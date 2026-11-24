/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;
use std::fmt;

use super::name::DomainName;
use super::types::ResourceType;

/// Why a message could not be decoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParseError {
    /// The input ended in the middle of a field, or a field ran past the end of its RDATA.
    UnexpectedEnd,
    /// A label length octet was above 63 (including the reserved 0b01 and 0b10 prefixes).
    LabelTooLong,
    /// A compression pointer did not point at a label of a previously decoded name.
    InvalidPointer,
    /// A domain name was longer than 255 octets.
    NameTooLong,
    /// A record's RDATA had octets left over after its fields.
    TrailingRecordData(ResourceType),
    /// A DNSKEY record had no public key.
    EmptyPublicKey,
    /// A DS record used a digest type of unknown size.
    /// A type bit map was malformed.
    InvalidTypeBitMap,
    /// An NSEC3 record had an empty next hashed owner name.
    EmptyNextHashedOwnerName,
    /// The OPT record's options did not fill its RDATA exactly.
    InvalidOpt,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnexpectedEnd => f.write_str("Unexpected end of DNS message"),
            Self::LabelTooLong => f.write_str("Domain name label exceeds 63 octets"),
            Self::InvalidPointer => f.write_str("Invalid domain name pointer in label"),
            Self::NameTooLong => f.write_str("Domain name exceeds 255 octets"),
            Self::TrailingRecordData(rtype) => write!(f, "Extra data in {rtype} record content"),
            Self::EmptyPublicKey => f.write_str("Empty public key in DNSKEY record"),
            Self::InvalidTypeBitMap => f.write_str("Invalid type bit map"),
            Self::EmptyNextHashedOwnerName => f.write_str("NSEC3 record has an empty next hashed owner name"),
            Self::InvalidOpt => f.write_str("Invalid OPT record"),
        }
    }
}

impl std::error::Error for ParseError {}

/// Why a message could not be encoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EncodeError {
    /// A domain name has an empty label, a label over 63 octets, or is over 255 octets.
    InvalidDomainName,
    /// A <character-string>, salt or hash is longer than its 8-bit length field allows.
    FieldTooLong,
    /// RDATA, an OPT option or a section is longer than its 16-bit length or count field allows.
    TooLarge,
    /// An OPT record whose type and data disagree.
    MismatchedOpt,
}

impl fmt::Display for EncodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidDomainName => f.write_str("Invalid domain name"),
            Self::FieldTooLong => f.write_str("Field exceeds 255 octets"),
            Self::TooLarge => f.write_str("Field exceeds 65535 octets or entries"),
            Self::MismatchedOpt => f.write_str("OPT record type and data disagree"),
        }
    }
}

impl std::error::Error for EncodeError {}

/// A bounds-checked cursor over a message. Positions are offsets from the start of the message, so that compression
/// pointers inside RDATA resolve against the same offsets as the rest of the message.
pub(crate) struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    pub(crate) fn new(buf: &'a [u8]) -> Self {
        Self { buf, pos: 0 }
    }

    pub(crate) fn position(&self) -> usize {
        self.pos
    }

    pub(crate) fn is_eof(&self) -> bool {
        self.pos >= self.buf.len()
    }

    pub(crate) fn bytes(&mut self, count: usize) -> Result<&'a [u8], ParseError> {
        let end = self.pos.checked_add(count).ok_or(ParseError::UnexpectedEnd)?;
        let bytes = self.buf.get(self.pos..end).ok_or(ParseError::UnexpectedEnd)?;
        self.pos = end;
        Ok(bytes)
    }

    pub(crate) fn array<const N: usize>(&mut self) -> Result<[u8; N], ParseError> {
        let mut array = [0; N];
        array.copy_from_slice(self.bytes(N)?);
        Ok(array)
    }

    pub(crate) fn u8(&mut self) -> Result<u8, ParseError> {
        Ok(self.array::<1>()?[0])
    }

    pub(crate) fn u16(&mut self) -> Result<u16, ParseError> {
        Ok(u16::from_be_bytes(self.array()?))
    }

    pub(crate) fn u32(&mut self) -> Result<u32, ParseError> {
        Ok(u32::from_be_bytes(self.array()?))
    }

    pub(crate) fn rest(&mut self) -> &'a [u8] {
        let rest = self.buf.get(self.pos..).unwrap_or_default();
        self.pos = self.buf.len();
        rest
    }

    /// Splits off the next `length` octets as a reader that cannot read past them, and skips over them.
    pub(crate) fn sub(&mut self, length: usize) -> Result<Reader<'a>, ParseError> {
        let start = self.pos;
        self.bytes(length)?;
        Ok(Reader {
            buf: self.buf.get(..self.pos).unwrap_or_default(),
            pos: start,
        })
    }

    /// The octets between `start` and the current position.
    pub(crate) fn consumed_since(&self, start: usize) -> &'a [u8] {
        self.buf.get(start..self.pos).unwrap_or_default()
    }
}

/// Names decoded so far, for resolving compression pointers.
#[derive(Default)]
pub(crate) struct ParseContext {
    pub(crate) names: Vec<DomainName>,
    /// Message offset of a label (or of the terminating root label, or of a pointer) -> (index into `names`,
    /// index of the label that starts there).
    pub(crate) label_offsets: HashMap<usize, (usize, usize)>,
}

pub(crate) fn put_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_be_bytes());
}

pub(crate) fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_be_bytes());
}

/// Writes an 8-bit length followed by `bytes`.
pub(crate) fn put_u8_prefixed(out: &mut Vec<u8>, bytes: &[u8]) -> Result<(), EncodeError> {
    out.push(u8::try_from(bytes.len()).map_err(|_| EncodeError::FieldTooLong)?);
    out.extend_from_slice(bytes);
    Ok(())
}

/// Lowercase hex without separators, like AK's `{:hex-dump}` for byte spans.
pub(crate) struct HexDump<'a>(pub(crate) &'a [u8]);

impl fmt::Display for HexDump<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.iter().try_for_each(|byte| write!(f, "{byte:02x}"))
    }
}

/// RFC 4648 base64 with padding.
pub(crate) struct Base64<'a>(pub(crate) &'a [u8]);

impl fmt::Display for Base64<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let symbol = |index: u32| char::from(ALPHABET[(index & 0x3f) as usize]);
        for chunk in self.0.chunks(3) {
            let b0 = u32::from(chunk[0]);
            let b1 = chunk.get(1).copied().map_or(0, u32::from);
            let b2 = chunk.get(2).copied().map_or(0, u32::from);
            let group = (b0 << 16) | (b1 << 8) | b2;
            write!(f, "{}{}", symbol(group >> 18), symbol(group >> 12))?;
            if chunk.len() > 1 {
                write!(f, "{}", symbol(group >> 6))?;
            } else {
                f.write_str("=")?;
            }
            if chunk.len() > 2 {
                write!(f, "{}", symbol(group))?;
            } else {
                f.write_str("=")?;
            }
        }
        Ok(())
    }
}

/// Seconds since the epoch as "YYYY-MM-DD HH:MM:SS" in UTC, like AK's UnixDateTime formatter.
pub(crate) struct UtcTime(pub(crate) u32);

impl fmt::Display for UtcTime {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let seconds = i64::from(self.0);
        let (days, rem) = (seconds / 86400, seconds % 86400);

        // Civil date from days since 1970-01-01 (proleptic Gregorian calendar).
        let z = days + 719_468;
        let era = z.div_euclid(146_097);
        let doe = z.rem_euclid(146_097);
        let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        let mp = (5 * doy + 2) / 153;
        let day = doy - (153 * mp + 2) / 5 + 1;
        let month = if mp < 10 { mp + 3 } else { mp - 9 };
        let year = yoe + era * 400 + i64::from(month <= 2);

        write!(
            f,
            "{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02}",
            rem / 3600,
            rem % 3600 / 60,
            rem % 60
        )
    }
}
