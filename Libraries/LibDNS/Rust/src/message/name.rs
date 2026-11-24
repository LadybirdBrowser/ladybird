/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cmp::Ordering;
use std::convert::Infallible;
use std::fmt;
use std::str::FromStr;

use super::wire::{EncodeError, ParseContext, ParseError, Reader};

/// A domain name as a list of labels, most specific first, without the root label.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
pub struct DomainName {
    pub labels: Vec<Vec<u8>>,
}

impl DomainName {
    pub fn root() -> Self {
        Self::default()
    }

    pub fn from_string(name: &str) -> Self {
        // RFC 1034, 3.1. Name space specifications and terminology.
        // Since a complete domain name ends with the root label, this leads to a printed form which ends in a dot.
        let name = name.strip_suffix('.').unwrap_or(name);
        if name.is_empty() {
            return Self::root();
        }
        Self {
            labels: name.split('.').map(|label| label.as_bytes().to_vec()).collect(),
        }
    }

    pub fn is_root(&self) -> bool {
        self.labels.is_empty()
    }

    pub fn is_valid(&self) -> bool {
        // RFC 1035, 2.3.4. Size limits.
        // labels          63 octets or less
        // names           255 octets or less
        let mut wire_length = 1;
        for label in &self.labels {
            if label.is_empty() || label.len() > 63 {
                return false;
            }
            wire_length += 1 + label.len();
        }
        wire_length <= 255
    }

    /// Appends the uncompressed wire form of the name.
    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        if !self.is_valid() {
            return Err(EncodeError::InvalidDomainName);
        }
        for label in &self.labels {
            out.push(label.len() as u8);
            out.extend_from_slice(label);
        }
        out.push(0);
        Ok(())
    }

    pub fn to_wire(&self) -> Result<Vec<u8>, EncodeError> {
        let mut out = Vec::new();
        self.write_wire(&mut out)?;
        Ok(out)
    }

    /// The presentation form with all labels lowercased, as raw octets.
    pub fn to_canonical_string(&self) -> Vec<u8> {
        if self.labels.is_empty() {
            return b".".to_vec();
        }

        // RFC 4034, 6.2. Canonical RR Form.
        // all uppercase US-ASCII letters in the owner name of the RR are replaced by the corresponding lowercase
        // US-ASCII letters;
        let mut out = Vec::new();
        for label in &self.labels {
            out.extend(label.iter().map(u8::to_ascii_lowercase));
            out.push(b'.');
        }
        out
    }

    /// RFC 4034, 6.2. Canonical RR Form: the name with uppercase US-ASCII letters replaced by lowercase ones.
    pub fn canonicalized(&self) -> Self {
        Self {
            labels: self.labels.iter().map(|label| label.to_ascii_lowercase()).collect(),
        }
    }

    /// The name with its first label removed. The root is its own parent.
    pub fn parent(&self) -> Self {
        Self {
            labels: self.labels.iter().skip(1).cloned().collect(),
        }
    }

    /// The name made of the rightmost `count` labels, or the whole name if it has fewer.
    pub fn suffix(&self, count: usize) -> Self {
        let start = self.labels.len().saturating_sub(count);
        Self {
            labels: self.labels.iter().skip(start).cloned().collect(),
        }
    }

    pub fn with_label_prepended(&self, label: impl Into<Vec<u8>>) -> Self {
        let mut labels = Vec::with_capacity(self.labels.len() + 1);
        labels.push(label.into());
        labels.extend(self.labels.iter().cloned());
        Self { labels }
    }

    pub fn is_wildcard(&self) -> bool {
        self.labels.first().is_some_and(|label| label == b"*")
    }

    pub fn eq_ignore_case(&self, other: &Self) -> bool {
        self.labels.len() == other.labels.len()
            && self
                .labels
                .iter()
                .zip(&other.labels)
                .all(|(a, b)| a.eq_ignore_ascii_case(b))
    }

    /// Whether this name is `other` or one of its ancestors.
    pub fn is_ancestor_or_equal_of(&self, other: &Self) -> bool {
        self.labels.len() <= other.labels.len() && self.common_suffix_length(other) == self.labels.len()
    }

    /// Number of trailing labels this name shares with `other`.
    pub fn common_suffix_length(&self, other: &Self) -> usize {
        self.labels
            .iter()
            .rev()
            .zip(other.labels.iter().rev())
            .take_while(|(a, b)| compare_labels(a, b) == Ordering::Equal)
            .count()
    }

    /// RFC 4034, 6.1. Canonical DNS Name Order.
    pub fn canonical_cmp(&self, other: &Self) -> Ordering {
        // To compute the canonical ordering of a set of DNS names, start by sorting the names according to their most
        // significant (rightmost) labels.  For names in which the most significant label is identical, continue sorting
        // according to their next most significant label, and so forth.
        for (a, b) in self.labels.iter().rev().zip(other.labels.iter().rev()) {
            let order = compare_labels(a, b);
            if order != Ordering::Equal {
                return order;
            }
        }
        self.labels.len().cmp(&other.labels.len())
    }

    pub(crate) fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 1035, 4.1.2. Question section format.
        // QNAME           a domain name represented as a sequence of labels, where
        //                each label consists of a length octet followed by that
        //                number of octets.  The domain name terminates with the
        //                zero length octet for the null label of the root.  Note
        //                that this field may be an odd number of octets; no
        //                padding is used.
        const OFFSET_MARKER_MASK: u8 = 0b1100_0000;

        let mut labels: Vec<Vec<u8>> = Vec::new();
        // Where each label of this name starts in the message, so later names can point at it.
        let mut offsets: Vec<(usize, usize)> = Vec::new();
        let mut wire_length = 1;
        loop {
            let offset = reader.position();
            let length = reader.u8()?;
            offsets.push((offset, labels.len()));
            if length == 0 {
                break;
            }

            if length & OFFSET_MARKER_MASK == OFFSET_MARKER_MASK {
                // RFC 1035, 4.1.4. Message compression
                // In order to reduce the size of messages, the domain system utilizes a
                // compression scheme which eliminates the repetition of domain names in a
                // message.  In this scheme, an entire domain name or a list of labels at
                // the end of a domain name is replaced with a pointer to a prior occurance
                // of the same name.
                // Only labels of names that have already been decoded in full are accepted as targets, which rules out
                // forward pointers, pointers into the name being decoded and pointer loops.
                let target = usize::from(length & !OFFSET_MARKER_MASK) << 8 | usize::from(reader.u8()?);
                let (name_index, label_index) = *ctx.label_offsets.get(&target).ok_or(ParseError::InvalidPointer)?;
                let suffix = ctx
                    .names
                    .get(name_index)
                    .and_then(|name| name.labels.get(label_index..))
                    .ok_or(ParseError::InvalidPointer)?;
                labels.extend_from_slice(suffix);
                break;
            }

            // A normal label is at most 63 octets. Lengths whose top two bits are 0b01 or 0b10 are reserved (RFC 1035,
            // 4.1.4) and would produce a label that can no longer be re-encoded, so reject them here.
            if length > 63 {
                return Err(ParseError::LabelTooLong);
            }

            wire_length += 1 + usize::from(length);
            if wire_length > 255 {
                return Err(ParseError::NameTooLong);
            }
            labels.push(reader.bytes(usize::from(length))?.to_vec());
        }

        let name = Self { labels };
        if !name.is_valid() {
            return Err(ParseError::NameTooLong);
        }

        let name_index = ctx.names.len();
        for (offset, label_index) in offsets {
            // Pointers carry a 14-bit offset.
            if offset <= 0x3fff {
                ctx.label_offsets.insert(offset, (name_index, label_index));
            }
        }
        ctx.names.push(name.clone());

        Ok(name)
    }
}

fn compare_labels(a: &[u8], b: &[u8]) -> Ordering {
    // RFC 4034, 6.1. Canonical DNS Name Order.
    // owner names are ordered by treating individual labels as unsigned left-justified octet strings.  The absence
    // of a octet sorts before a zero value octet, and uppercase US-ASCII letters are treated as if they were
    // lowercase US-ASCII letters.
    a.iter()
        .map(u8::to_ascii_lowercase)
        .cmp(b.iter().map(u8::to_ascii_lowercase))
}

impl fmt::Display for DomainName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.labels.is_empty() {
            return f.write_str(".");
        }
        for label in &self.labels {
            write!(f, "{}.", String::from_utf8_lossy(label))?;
        }
        Ok(())
    }
}

impl FromStr for DomainName {
    type Err = Infallible;

    fn from_str(name: &str) -> Result<Self, Self::Err> {
        Ok(Self::from_string(name))
    }
}
