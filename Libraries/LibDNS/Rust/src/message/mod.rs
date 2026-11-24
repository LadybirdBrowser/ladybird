/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The DNS message wire format (RFC 1035, 4.) and the record types LibDNS understands.

mod name;
mod records;
mod types;
mod wire;

#[cfg(test)]
mod tests;

use std::fmt;

pub use name::DomainName;
pub use records::{
    Dnskey, Ds, Hinfo, Mx, Nsec, Nsec3, Nsec3Param, Opt, OptOption, RecordData, RsaPublicKeyComponents, Sig, Soa, Srv,
    Tlsa, Txt, type_bit_maps_from_wire, type_bit_maps_to_wire,
};
pub use types::{
    Algorithm, Class, DigestType, Nsec3HashAlgorithm, OpCode, ResourceType, ResponseCode, TlsaCertUsage,
    TlsaMatchingType, TlsaSelector,
};
pub use wire::{EncodeError, ParseError};

use wire::{ParseContext, Reader, put_u16, put_u32};

/// The second 16-bit word of the header.
//                                  1  1  1  1  1  1
//    0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//    |                      ID                       |
//    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//    |QR| Opcode |AA|TC|RD|RA|   Z |AD|CD|   RCODE   |
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Options(pub u16);

impl Options {
    pub const QR_MASK: u16 = 0b1000_0000_0000_0000;
    pub const OP_CODE_MASK: u16 = 0b0111_1000_0000_0000;
    pub const AUTHORITATIVE_ANSWER_MASK: u16 = 0b0000_0100_0000_0000;
    pub const TRUNCATED_MASK: u16 = 0b0000_0010_0000_0000;
    pub const RECURSION_DESIRED_MASK: u16 = 0b0000_0001_0000_0000;
    pub const RECURSION_AVAILABLE_MASK: u16 = 0b0000_0000_1000_0000;
    pub const AUTHENTICATED_DATA_MASK: u16 = 0b0000_0000_0010_0000;
    pub const CHECKING_DISABLED_MASK: u16 = 0b0000_0000_0001_0000;
    pub const RESPONSE_CODE_MASK: u16 = 0b0000_0000_0000_1111;

    fn set_bit(&mut self, mask: u16, value: bool) {
        self.0 = (self.0 & !mask) | if value { mask } else { 0 };
    }

    pub fn set_is_question(&mut self, value: bool) {
        self.set_bit(Self::QR_MASK, !value);
    }

    pub fn set_is_authoritative_answer(&mut self, value: bool) {
        self.set_bit(Self::AUTHORITATIVE_ANSWER_MASK, value);
    }

    pub fn set_is_truncated(&mut self, value: bool) {
        self.set_bit(Self::TRUNCATED_MASK, value);
    }

    pub fn set_recursion_desired(&mut self, value: bool) {
        self.set_bit(Self::RECURSION_DESIRED_MASK, value);
    }

    pub fn set_recursion_available(&mut self, value: bool) {
        self.set_bit(Self::RECURSION_AVAILABLE_MASK, value);
    }

    pub fn set_checking_disabled(&mut self, value: bool) {
        self.set_bit(Self::CHECKING_DISABLED_MASK, value);
    }

    pub fn set_authenticated_data(&mut self, value: bool) {
        self.set_bit(Self::AUTHENTICATED_DATA_MASK, value);
    }

    /// Only the low four bits of `code` fit in the header.
    pub fn set_response_code(&mut self, code: ResponseCode) {
        self.0 = (self.0 & !Self::RESPONSE_CODE_MASK) | (code.0 & Self::RESPONSE_CODE_MASK);
    }

    /// Only the low four bits of `code` fit in the header.
    pub fn set_op_code(&mut self, code: OpCode) {
        self.0 = (self.0 & !Self::OP_CODE_MASK) | ((u16::from(code.0) << 11) & Self::OP_CODE_MASK);
    }

    pub const fn is_question(self) -> bool {
        self.0 & Self::QR_MASK == 0
    }

    pub const fn is_authoritative_answer(self) -> bool {
        self.0 & Self::AUTHORITATIVE_ANSWER_MASK != 0
    }

    pub const fn is_truncated(self) -> bool {
        self.0 & Self::TRUNCATED_MASK != 0
    }

    pub const fn recursion_desired(self) -> bool {
        self.0 & Self::RECURSION_DESIRED_MASK != 0
    }

    pub const fn recursion_available(self) -> bool {
        self.0 & Self::RECURSION_AVAILABLE_MASK != 0
    }

    pub const fn checking_disabled(self) -> bool {
        self.0 & Self::CHECKING_DISABLED_MASK != 0
    }

    pub const fn authenticated_data(self) -> bool {
        self.0 & Self::AUTHENTICATED_DATA_MASK != 0
    }

    pub const fn response_code(self) -> ResponseCode {
        ResponseCode(self.0 & Self::RESPONSE_CODE_MASK)
    }

    pub const fn op_code(self) -> OpCode {
        OpCode(((self.0 & Self::OP_CODE_MASK) >> 11) as u8)
    }
}

impl fmt::Display for Options {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "QR: {}, Opcode: {}, AA: {}, TC: {}, RD: {}, RA: {}, AD: {}, CD: {}, RCODE: {}",
            if self.is_question() { "Q" } else { "R" },
            self.op_code(),
            self.is_authoritative_answer(),
            self.is_truncated(),
            self.recursion_desired(),
            self.recursion_available(),
            self.authenticated_data(),
            self.checking_disabled(),
            self.response_code()
        )
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Header {
    pub id: u16,
    pub options: Options,
    pub question_count: u16,
    pub answer_count: u16,
    pub authority_count: u16,
    pub additional_count: u16,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Question {
    pub name: DomainName,
    pub rtype: ResourceType,
    pub class: Class,
}

impl Question {
    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 1035, 4.1.2. Question section format.
        // +        +
        // | QNAME  | a domain name represented as a sequence of labels
        // +        +
        // | QTYPE  | a two octet code which specifies the type of the query
        // | QCLASS | a two octet code that specifies the class of the query
        Ok(Self {
            name: DomainName::parse(reader, ctx)?,
            rtype: ResourceType(reader.u16()?),
            class: Class(reader.u16()?),
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        self.name.write_wire(out)?;
        put_u16(out, self.rtype.0);
        put_u16(out, self.class.0);
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ResourceRecord {
    pub name: DomainName,
    pub rtype: ResourceType,
    /// Always IN for OPT, whose CLASS field carries the UDP payload size instead.
    pub class: Class,
    /// Always 0 for OPT, whose TTL field carries the extended RCODE and flags instead.
    pub ttl: u32,
    pub data: RecordData,
    /// The record exactly as it appeared in a parsed message, from the start of its owner name to the end of its
    /// RDATA. Compression pointers are left as they were, so this is only meaningful together with that message.
    /// `None` for records that were not parsed.
    pub raw: Option<Vec<u8>>,
}

impl ResourceRecord {
    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 1035, 4.1.3. Resource record format.
        // +           +
        // | NAME      | a domain name to which this resource record pertains
        // +           +
        // | TYPE      | two octets containing one of the RR type codes
        // | CLASS     | two octets containing one of the RR class codes
        // | TTL       | a 32-bit unsigned integer that specifies the time interval
        // |           | that the resource record may be cached
        // | RDLENGTH  | an unsigned 16-bit integer that specifies the length in
        // |           | octets of the RDATA field
        // | RDATA     | a variable length string of octets that describes the resource
        let start = reader.position();
        let name = DomainName::parse(reader, ctx)?;
        let rtype = ResourceType(reader.u16()?);
        if rtype == ResourceType::OPT {
            let opt = Opt::parse(reader)?;
            return Ok(Self {
                name,
                rtype,
                class: Class::IN,
                ttl: 0,
                data: RecordData::Opt(opt),
                raw: Some(reader.consumed_since(start).to_vec()),
            });
        }

        let class = Class(reader.u16()?);
        // RFC 2181, 8. Time to Live (TTL)
        // Implementations should treat TTL values received with the most significant bit set as if the entire value
        // received was zero.
        let ttl = match reader.u32()? {
            ttl if ttl & 0x8000_0000 != 0 => 0,
            ttl => ttl,
        };
        let rd_length = reader.u16()?;
        let mut rdata = reader.sub(usize::from(rd_length))?;
        let data = RecordData::parse(rtype, &mut rdata, ctx)?;
        if !rdata.is_eof() {
            return Err(ParseError::TrailingRecordData(rtype));
        }

        Ok(Self {
            name,
            rtype,
            class,
            ttl,
            data,
            raw: Some(reader.consumed_since(start).to_vec()),
        })
    }

    /// Appends the record without name compression.
    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        self.name.write_wire(out)?;
        put_u16(out, self.rtype.0);

        match (&self.data, self.rtype == ResourceType::OPT) {
            (RecordData::Opt(opt), true) => opt.write_wire(out),
            (RecordData::Opt(_), false) | (_, true) => Err(EncodeError::MismatchedOpt),
            (data, false) => {
                put_u16(out, self.class.0);
                put_u32(out, self.ttl);
                let length_offset = out.len();
                put_u16(out, 0);
                data.write_rdata(out)?;
                let rd_length = u16::try_from(out.len() - length_offset - 2).map_err(|_| EncodeError::TooLarge)?;
                out[length_offset..length_offset + 2].copy_from_slice(&rd_length.to_be_bytes());
                Ok(())
            }
        }
    }

    /// RFC 4034, 6.2. Canonical RR Form, with the given owner name (already canonicalized or wildcard-substituted)
    /// and TTL.
    pub fn write_canonical_wire(&self, out: &mut Vec<u8>, owner: &DomainName, ttl: u32) -> Result<(), EncodeError> {
        let canonical = Self {
            name: owner.clone(),
            rtype: self.rtype,
            class: self.class,
            ttl,
            data: self.data.canonicalized(),
            raw: None,
        };
        canonical.write_wire(out)
    }
}

impl fmt::Display for ResourceRecord {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "[{} {} {} | ttl={}, name={}]",
            self.class, self.rtype, self.data, self.ttl, self.name
        )
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Message {
    pub header: Header,
    pub questions: Vec<Question>,
    pub answers: Vec<ResourceRecord>,
    pub authorities: Vec<ResourceRecord>,
    pub additional_records: Vec<ResourceRecord>,
}

impl Message {
    /// Decodes a message. Octets after the last record announced by the header are ignored.
    pub fn from_wire(bytes: &[u8]) -> Result<Self, ParseError> {
        // RFC 1035, 4.1. (Messages) Format.
        // | Header      |
        // | Question    | the question for the name server
        // | Answer      | RRs answering the question
        // | Authority   | RRs pointing toward an authority
        // | Additional  | RRs holding additional information
        //
        // The header section is always present.  The header includes fields that
        // specify which of the remaining sections are present, and also specify
        // whether the message is a query or a response, a standard query or some
        // other opcode, etc.
        let mut reader = Reader::new(bytes);
        let mut ctx = ParseContext::default();

        let header = Header {
            id: reader.u16()?,
            options: Options(reader.u16()?),
            question_count: reader.u16()?,
            answer_count: reader.u16()?,
            authority_count: reader.u16()?,
            additional_count: reader.u16()?,
        };

        // The counts are untrusted, so the sections grow only as records are actually decoded.
        let mut message = Self {
            header,
            ..Self::default()
        };
        for _ in 0..header.question_count {
            message.questions.push(Question::parse(&mut reader, &mut ctx)?);
        }
        for _ in 0..header.answer_count {
            message.answers.push(ResourceRecord::parse(&mut reader, &mut ctx)?);
        }
        for _ in 0..header.authority_count {
            message.authorities.push(ResourceRecord::parse(&mut reader, &mut ctx)?);
        }
        for _ in 0..header.additional_count {
            message
                .additional_records
                .push(ResourceRecord::parse(&mut reader, &mut ctx)?);
        }

        Ok(message)
    }

    /// Encodes the message without name compression. The section counts in the header are taken from the sections
    /// themselves, not from `header`.
    pub fn to_wire(&self) -> Result<Vec<u8>, EncodeError> {
        let count = |length: usize| u16::try_from(length).map_err(|_| EncodeError::TooLarge);

        let mut out = Vec::new();
        put_u16(&mut out, self.header.id);
        put_u16(&mut out, self.header.options.0);
        put_u16(&mut out, count(self.questions.len())?);
        put_u16(&mut out, count(self.answers.len())?);
        put_u16(&mut out, count(self.authorities.len())?);
        put_u16(&mut out, count(self.additional_records.len())?);

        for question in &self.questions {
            question.write_wire(&mut out)?;
        }
        for record in self
            .answers
            .iter()
            .chain(&self.authorities)
            .chain(&self.additional_records)
        {
            record.write_wire(&mut out)?;
        }
        Ok(out)
    }

    /// The answer records that respond to the question: RRs owned by the queried name, and RRs reached through a
    /// chain of CNAMEs starting there, wherever in the section they are. Anything an alias leads to, including the
    /// next alias, is valid for no longer than the alias.
    pub fn answers_to(&self, question: &Question) -> Vec<ResourceRecord> {
        // Each owner along the chain, with how long what it owns may live.
        let mut owners = vec![(question.name.clone(), u32::MAX)];
        let mut accepted = Vec::new();

        let mut next = 0;
        while let Some((owner, ttl_limit)) = owners.get(next).cloned() {
            next += 1;
            // RFC 2181, 5.2. TTLs of RRs in an RRSet
            // Should an authoritative source send such a malformed RRSet, the client should treat the RRs for all
            // purposes as if all TTLs in the RRSet had been set to the value of the lowest TTL in the RRSet.
            let alias_ttl = self
                .answers
                .iter()
                .filter(|record| {
                    record.rtype == ResourceType::CNAME
                        && record.class == question.class
                        && owner.eq_ignore_case(&record.name)
                })
                .map(|record| record.ttl)
                .min()
                .unwrap_or(u32::MAX);
            for record in &self.answers {
                if record.class != question.class || !owner.eq_ignore_case(&record.name) {
                    continue;
                }

                let rtype = match &record.data {
                    RecordData::Rrsig(rrsig) if record.rtype == ResourceType::RRSIG => rrsig.type_covered,
                    _ => record.rtype,
                };
                let is_alias = rtype == ResourceType::CNAME && question.rtype != ResourceType::CNAME;
                if !is_alias && rtype != question.rtype && question.rtype != ResourceType::ANY {
                    continue;
                }

                if is_alias
                    && let (ResourceType::CNAME, RecordData::Cname(target)) = (record.rtype, &record.data)
                    && !owners.iter().any(|(owner, _)| owner.eq_ignore_case(target))
                {
                    owners.push((target.clone(), ttl_limit.min(alias_ttl)));
                }

                let mut copy = record.clone();
                copy.ttl = copy.ttl.min(ttl_limit);
                accepted.push(copy);
            }
        }
        accepted
    }
}
