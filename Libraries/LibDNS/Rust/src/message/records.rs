/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fmt;
use std::net::{Ipv4Addr, Ipv6Addr};

use super::name::DomainName;
use super::types::{
    Algorithm, DigestType, Nsec3HashAlgorithm, ResourceType, TlsaCertUsage, TlsaMatchingType, TlsaSelector,
};
use super::wire::{
    Base64, EncodeError, HexDump, ParseContext, ParseError, Reader, UtcTime, put_u8_prefixed, put_u16, put_u32,
};

/// The RDATA of a resource record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RecordData {
    A(Ipv4Addr),
    Aaaa(Ipv6Addr),
    Txt(Txt),
    Cname(DomainName),
    Ns(DomainName),
    Soa(Soa),
    Mx(Mx),
    Ptr(DomainName),
    Dname(DomainName),
    Srv(Srv),
    Dnskey(Dnskey),
    Cdnskey(Dnskey),
    Ds(Ds),
    Cds(Ds),
    Rrsig(Sig),
    Nsec(Nsec),
    Nsec3(Nsec3),
    Nsec3Param(Nsec3Param),
    Tlsa(Tlsa),
    Hinfo(Hinfo),
    Opt(Opt),
    /// Fallback for unknown records.
    Unknown(Vec<u8>),
}

impl RecordData {
    /// Decodes the RDATA of a record of type `rtype`. `reader` is bounded to the RDATA. OPT is not handled here, as
    /// it does not use the standard RR layout.
    pub(crate) fn parse(
        rtype: ResourceType,
        reader: &mut Reader<'_>,
        ctx: &mut ParseContext,
    ) -> Result<Self, ParseError> {
        Ok(match rtype {
            ResourceType::A => {
                // RFC 1035, 3.4.1. A RDATA format.
                // | ADDRESS | a 32 bit Internet address.
                Self::A(Ipv4Addr::from(reader.array::<4>()?))
            }
            ResourceType::AAAA => {
                // RFC 3596, 2.2. AAAA RDATA format.
                // | ADDRESS | a 128 bit Internet address.
                Self::Aaaa(Ipv6Addr::from(reader.array::<16>()?))
            }
            ResourceType::TXT => Self::Txt(Txt::parse(reader)?),
            ResourceType::CNAME => {
                // RFC 1035, 3.3.1. CNAME RDATA format.
                // | CNAME | a <domain-name> which specifies the canonical or primary name for the owner.
                Self::Cname(DomainName::parse(reader, ctx)?)
            }
            ResourceType::NS => {
                // RFC 1035, 3.3.11. NS RDATA format.
                // | NSDNAME | a <domain-name> which specifies a host which should be authoritative for the specified class and domain.
                Self::Ns(DomainName::parse(reader, ctx)?)
            }
            ResourceType::SOA => Self::Soa(Soa::parse(reader, ctx)?),
            ResourceType::MX => Self::Mx(Mx::parse(reader, ctx)?),
            ResourceType::PTR => {
                // RFC 1035, 3.3.12. PTR RDATA format.
                // | PTRDNAME | a <domain-name> which points to some location in the domain name space.
                Self::Ptr(DomainName::parse(reader, ctx)?)
            }
            ResourceType::DNAME => {
                // RFC 6672, 2.1. Format.
                // Its RDATA is comprised of a single field, <target>, which contains a fully qualified domain name
                // that MUST be sent in uncompressed form [RFC1035] [RFC3597].
                // A compressed one is still read.
                Self::Dname(DomainName::parse(reader, ctx)?)
            }
            ResourceType::SRV => Self::Srv(Srv::parse(reader, ctx)?),
            ResourceType::DNSKEY => Self::Dnskey(Dnskey::parse(reader)?),
            ResourceType::CDNSKEY => Self::Cdnskey(Dnskey::parse(reader)?),
            ResourceType::DS => Self::Ds(Ds::parse(reader)?),
            ResourceType::CDS => Self::Cds(Ds::parse(reader)?),
            ResourceType::RRSIG => Self::Rrsig(Sig::parse(reader, ctx)?),
            ResourceType::NSEC => Self::Nsec(Nsec::parse(reader, ctx)?),
            ResourceType::NSEC3 => Self::Nsec3(Nsec3::parse(reader)?),
            ResourceType::NSEC3PARAM => Self::Nsec3Param(Nsec3Param::parse(reader)?),
            ResourceType::TLSA => Self::Tlsa(Tlsa::parse(reader)?),
            ResourceType::HINFO => Self::Hinfo(Hinfo::parse(reader)?),
            _ => Self::Unknown(reader.rest().to_vec()),
        })
    }

    /// Appends the RDATA. For OPT this is everything after TYPE, see `Opt::write_wire`.
    pub fn write_rdata(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        match self {
            Self::A(address) => out.extend_from_slice(&address.octets()),
            Self::Aaaa(address) => out.extend_from_slice(&address.octets()),
            Self::Txt(txt) => txt.write_wire(out)?,
            // RFC 6672, 2.5. Compression of the DNAME Record.
            // The DNAME RDATA target name MUST NOT be sent out in compressed form
            // No name is compressed when written.
            Self::Cname(name) | Self::Ns(name) | Self::Ptr(name) | Self::Dname(name) => name.write_wire(out)?,
            Self::Soa(soa) => soa.write_wire(out)?,
            Self::Mx(mx) => mx.write_wire(out)?,
            Self::Srv(srv) => srv.write_wire(out)?,
            Self::Dnskey(key) | Self::Cdnskey(key) => key.write_wire(out),
            Self::Ds(ds) | Self::Cds(ds) => ds.write_wire(out),
            Self::Rrsig(sig) => sig.write_wire(out)?,
            Self::Nsec(nsec) => nsec.write_wire(out)?,
            Self::Nsec3(nsec3) => nsec3.write_wire(out)?,
            Self::Nsec3Param(param) => param.write_wire(out)?,
            Self::Tlsa(tlsa) => tlsa.write_wire(out),
            Self::Hinfo(hinfo) => hinfo.write_wire(out)?,
            Self::Opt(opt) => opt.write_wire(out)?,
            Self::Unknown(raw) => out.extend_from_slice(raw),
        }
        Ok(())
    }

    /// RFC 4034, 6.2. Canonical RR Form: the RDATA with the DNS names it contains lowercased, for the types that call
    /// for it.
    pub fn canonicalized(&self) -> Self {
        // RFC 4034, 6.2. Canonical RR Form.
        // 3.  if the type of the RR is NS, MD, MF, CNAME, SOA, MB, MG, MR, PTR, HINFO, MINFO, MX, HINFO, RP, AFSDB, RT,
        //     SIG, PX, NXT, NAPTR, KX, SRV, DNAME, A6, RRSIG, or NSEC, all uppercase US-ASCII letters in the DNS names
        //     contained within the RDATA are replaced by the corresponding lowercase US-ASCII letters;
        // RFC 6840, 5.1. Errors in Canonical Form Type Code List.
        // DNS names in the RDATA section of NSEC resource records are not converted to lowercase.  DNS names in the RDATA
        // section of RRSIG resource records are converted to lowercase.
        let mut data = self.clone();
        match &mut data {
            Self::Ns(name) | Self::Cname(name) | Self::Ptr(name) | Self::Dname(name) => *name = name.canonicalized(),
            Self::Soa(soa) => {
                soa.mname = soa.mname.canonicalized();
                soa.rname = soa.rname.canonicalized();
            }
            Self::Mx(mx) => mx.exchange = mx.exchange.canonicalized(),
            Self::Srv(srv) => srv.target = srv.target.canonicalized(),
            Self::Rrsig(rrsig) => rrsig.signers_name = rrsig.signers_name.canonicalized(),
            _ => {}
        }
        data
    }
}

impl fmt::Display for RecordData {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::A(address) => write!(f, "{address}"),
            Self::Aaaa(address) => write!(f, "{address}"),
            Self::Txt(txt) => write!(f, "{txt}"),
            Self::Cname(name) | Self::Ns(name) | Self::Ptr(name) | Self::Dname(name) => write!(f, "{name}"),
            Self::Soa(soa) => write!(f, "{soa}"),
            Self::Mx(mx) => write!(f, "{mx}"),
            Self::Srv(srv) => write!(f, "{srv}"),
            Self::Dnskey(key) | Self::Cdnskey(key) => write!(f, "{key}"),
            Self::Ds(ds) | Self::Cds(ds) => write!(f, "{ds}"),
            Self::Rrsig(sig) => write!(f, "{sig}"),
            Self::Nsec(nsec) => write!(f, "{nsec}"),
            Self::Nsec3(nsec3) => write!(f, "{nsec3}"),
            Self::Nsec3Param(param) => write!(f, "{param}"),
            Self::Tlsa(tlsa) => write!(f, "{tlsa}"),
            Self::Hinfo(hinfo) => write!(f, "{hinfo}"),
            Self::Opt(opt) => write!(f, "{opt}"),
            Self::Unknown(raw) => write!(f, "{}", HexDump(raw)),
        }
    }
}

/// Reads an 8-bit length followed by that many octets.
fn character_string(reader: &mut Reader<'_>) -> Result<Vec<u8>, ParseError> {
    let length = reader.u8()?;
    Ok(reader.bytes(usize::from(length))?.to_vec())
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Txt {
    pub strings: Vec<Vec<u8>>,
}

impl Txt {
    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 1035, 3.3.14. TXT RDATA format.
        // | TXT-DATA | One or more <character-string>s.
        let mut strings = vec![character_string(reader)?];
        while !reader.is_eof() {
            strings.push(character_string(reader)?);
        }
        Ok(Self { strings })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        if self.strings.is_empty() {
            return put_u8_prefixed(out, &[]);
        }
        for string in &self.strings {
            put_u8_prefixed(out, string)?;
        }
        Ok(())
    }
}

impl fmt::Display for Txt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Text:")?;
        for string in &self.strings {
            write!(f, " '{}'", String::from_utf8_lossy(string))?;
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Soa {
    pub mname: DomainName,
    pub rname: DomainName,
    pub serial: u32,
    pub refresh: u32,
    pub retry: u32,
    pub expire: u32,
    pub minimum: u32,
}

impl Soa {
    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 1035, 3.3.13. SOA RDATA format.
        // | MNAME   | <domain-name> which specifies the name of the host where the master file for the zone is maintained.
        // | RNAME   | <domain-name> which specifies the mailbox of the person responsible for this zone.
        // | SERIAL  | a 32-bit unsigned integer that specifies the version number of the original copy of the zone.
        // | REFRESH | a 32-bit unsigned integer that specifies the time interval before the zone should be refreshed.
        // | RETRY   | a 32-bit unsigned integer that specifies the time interval that should elapse before a failed refresh should be retried.
        // | EXPIRE  | a 32-bit unsigned integer that specifies the time value that specifies the upper limit on the time interval that can elapse before the zone is no longer authoritative.
        // | MINIMUM | a 32-bit unsigned integer that specifies the minimum TTL field that should be exported with any RR from this zone.
        Ok(Self {
            mname: DomainName::parse(reader, ctx)?,
            rname: DomainName::parse(reader, ctx)?,
            serial: reader.u32()?,
            refresh: reader.u32()?,
            retry: reader.u32()?,
            expire: reader.u32()?,
            minimum: reader.u32()?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        self.mname.write_wire(out)?;
        self.rname.write_wire(out)?;
        for value in [self.serial, self.refresh, self.retry, self.expire, self.minimum] {
            put_u32(out, value);
        }
        Ok(())
    }
}

impl fmt::Display for Soa {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SOA MName: '{}', RName: '{}', Serial: {}, Refresh: {}, Retry: {}, Expire: {}, Minimum: {}",
            self.mname, self.rname, self.serial, self.refresh, self.retry, self.expire, self.minimum
        )
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Mx {
    pub preference: u16,
    pub exchange: DomainName,
}

impl Mx {
    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 1035, 3.3.9. MX RDATA format.
        // | PREFERENCE | a 16 bit integer which specifies the preference given to this RR among others at the same owner.
        // | EXCHANGE   | a <domain-name> which specifies a host willing to act as a mail exchange for the owner name.
        Ok(Self {
            preference: reader.u16()?,
            exchange: DomainName::parse(reader, ctx)?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        put_u16(out, self.preference);
        self.exchange.write_wire(out)
    }
}

impl fmt::Display for Mx {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "MX Preference: {}, Exchange: '{}'", self.preference, self.exchange)
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Srv {
    pub priority: u16,
    pub weight: u16,
    pub port: u16,
    pub target: DomainName,
}

impl Srv {
    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 2782, 2. Service location and priority.
        // | PRIORITY | a 16 bit integer that specifies the priority of this target host.
        // | WEIGHT   | a 16 bit integer that specifies a weight for this target host.
        // | PORT     | a 16 bit integer that specifies the port on this target host.
        // | TARGET   | a <domain-name> which specifies the target host.
        Ok(Self {
            priority: reader.u16()?,
            weight: reader.u16()?,
            port: reader.u16()?,
            target: DomainName::parse(reader, ctx)?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        put_u16(out, self.priority);
        put_u16(out, self.weight);
        put_u16(out, self.port);
        self.target.write_wire(out)
    }
}

impl fmt::Display for Srv {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SRV Priority: {}, Weight: {}, Port: {}, Target: '{}'",
            self.priority, self.weight, self.port, self.target
        )
    }
}

/// DNSKEY and CDNSKEY RDATA.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Dnskey {
    pub flags: u16,
    pub protocol: u8,
    pub algorithm: Algorithm,
    pub public_key: Vec<u8>,
}

/// Public key components (pointing into `Dnskey::public_key`), for RSA keys only.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RsaPublicKeyComponents<'a> {
    pub exponent: &'a [u8],
    pub modulus: &'a [u8],
}

impl Dnskey {
    // RFC 4034, 2.1.1. The Flags Field: bit 7 is the Zone Key flag, bit 15 the Secure Entry Point flag, counting
    // from the most significant bit. RFC 5011, 2.1: bit 8 is the REVOKE flag.
    pub const FLAG_ZONE_KEY: u16 = 0b0000_0001_0000_0000;
    pub const FLAG_REVOKED: u16 = 0b0000_0000_1000_0000;
    pub const FLAG_SECURE_ENTRY_POINT: u16 = 0b0000_0000_0000_0001;

    pub const fn is_secure_entry_point(&self) -> bool {
        self.flags & Self::FLAG_SECURE_ENTRY_POINT != 0
    }

    pub const fn is_zone_key(&self) -> bool {
        self.flags & Self::FLAG_ZONE_KEY != 0
    }

    pub const fn is_revoked(&self) -> bool {
        self.flags & Self::FLAG_REVOKED != 0
    }

    pub const fn is_key_signing_key(&self) -> bool {
        self.is_secure_entry_point() && self.is_zone_key() && !self.is_revoked()
    }

    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 4034, 2.1. The DNSKEY Resource Record.
        // | FLAGS    | a 16-bit value that flags the key.
        // | PROTOCOL | an 8-bit value that specifies the protocol for this key.
        // | ALGORITHM| an 8-bit value that identifies the public key's cryptographic algorithm.
        // | PUBLICKEY| the public key material.
        let flags = reader.u16()?;
        let protocol = reader.u8()?;
        let algorithm = Algorithm(reader.u8()?);
        let public_key = reader.rest().to_vec();
        if public_key.is_empty() {
            return Err(ParseError::EmptyPublicKey);
        }
        Ok(Self {
            flags,
            protocol,
            algorithm,
            public_key,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) {
        put_u16(out, self.flags);
        out.push(self.protocol);
        out.push(self.algorithm.0);
        out.extend_from_slice(&self.public_key);
    }

    /// RFC 4034, Appendix B. Key Tag Calculation, over this key's RDATA.
    pub fn calculated_key_tag(&self) -> u16 {
        let mut rdata = Vec::with_capacity(4 + self.public_key.len());
        self.write_wire(&mut rdata);
        let mut accumulator: u32 = 0;
        for (index, byte) in rdata.iter().enumerate() {
            accumulator += if index & 1 == 1 {
                u32::from(*byte)
            } else {
                u32::from(*byte) << 8
            };
        }
        accumulator += (accumulator >> 16) & 0xffff;
        (accumulator & 0xffff) as u16
    }

    pub fn rsa_public_key_components(&self) -> Option<RsaPublicKeyComponents<'_>> {
        // RFC 3110, 2. RSA Public KEY Resource Records.
        // | exponent length | 1 or 3 octets (see text)
        // | exponent        | as specified by length field
        // | modulus         | remaining space
        // The public key exponent is a variable length unsigned integer.  Its length in octets is represented as one
        // octet if it is in the range of 1 to 255 and by a zero octet followed by a two octet unsigned length if it is
        // longer than 255 bytes.
        let bytes = self.public_key.as_slice();
        let (exponent_length, exponent_offset) = match bytes {
            [] => return None,
            [0, high, low, ..] => (usize::from(*high) << 8 | usize::from(*low), 3),
            [0, ..] => return None,
            [length, ..] => (usize::from(*length), 1),
        };

        if exponent_length == 0 || bytes.len() <= exponent_offset + exponent_length {
            return None;
        }

        let (exponent, modulus) = bytes.get(exponent_offset..)?.split_at(exponent_length);
        Some(RsaPublicKeyComponents { exponent, modulus })
    }
}

impl fmt::Display for Dnskey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "DNSKEY Flags: {}{}{}{}({}), Protocol: {}, Algorithm: {}, Public Key: {}, Tag: {}",
            if self.is_secure_entry_point() { "sep " } else { "" },
            if self.is_zone_key() { "zone " } else { "" },
            if self.is_revoked() { "revoked " } else { "" },
            if self.is_key_signing_key() { "ksk " } else { "" },
            self.flags,
            self.protocol,
            self.algorithm,
            Base64(&self.public_key),
            self.calculated_key_tag()
        )
    }
}

/// DS and CDS RDATA.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Ds {
    pub key_tag: u16,
    pub algorithm: Algorithm,
    pub digest_type: DigestType,
    pub digest: Vec<u8>,
}

impl Ds {
    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 4034, 5.1. The DS Resource Record.
        // | KEYTAG      | a 16-bit value that identifies the DNSKEY RR.
        // | ALGORITHM   | an 8-bit value that identifies the DS's hash algorithm.
        // | DIGEST TYPE | an 8-bit value that identifies the DS's digest algorithm.
        // | DIGEST      | Digest of the DNSKEY RDATA (Flags | Protocol | Algorithm | Pubkey).
        let key_tag = reader.u16()?;
        let algorithm = Algorithm(reader.u8()?);
        let digest_type = DigestType(reader.u8()?);
        let digest_size = match digest_type {
            DigestType::SHA1 => 20,
            DigestType::SHA256 | DigestType::GOST3411 => 32,
            DigestType::SHA384 => 48,
            DigestType::SHA512 => 64,
            DigestType::SHA224 => 28,
            // RFC 4035, 5.2. Authenticating Referrals.
            // If the validator does not support any of the algorithms listed in an authenticated DS RRset, then the
            // resolver has no supported authentication path leading from the parent to the child.
            // The validator decides what to do with such a DS; it is no reason to reject the message carrying it.
            _ => {
                return Ok(Self {
                    key_tag,
                    algorithm,
                    digest_type,
                    digest: reader.rest().to_vec(),
                });
            }
        };
        Ok(Self {
            key_tag,
            algorithm,
            digest_type,
            digest: reader.bytes(digest_size)?.to_vec(),
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) {
        put_u16(out, self.key_tag);
        out.push(self.algorithm.0);
        out.push(self.digest_type.0);
        out.extend_from_slice(&self.digest);
    }
}

impl fmt::Display for Ds {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "DS Key Tag: {}, Algorithm: {}, Digest Type: {}, Digest: {}",
            self.key_tag,
            self.algorithm,
            self.digest_type,
            Base64(&self.digest)
        )
    }
}

/// SIG and RRSIG RDATA.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Sig {
    pub type_covered: ResourceType,
    pub algorithm: Algorithm,
    pub label_count: u8,
    pub original_ttl: u32,
    /// Seconds since the epoch.
    pub expiration: u32,
    /// Seconds since the epoch.
    pub inception: u32,
    pub key_tag: u16,
    pub signers_name: DomainName,
    pub signature: Vec<u8>,
}

impl Sig {
    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 4034, 2.2. The SIG Resource Record.
        // | TYPE-COVERED | a 16-bit value that specifies the type of the RRset that is covered by this SIG.
        // | ALGORITHM    | an 8-bit value that specifies the algorithm used to create the signature.
        // | LABELS       | an 8-bit value that specifies the number of labels in the original SIG RR owner name.
        // | ORIGINAL TTL | a 32-bit value that specifies the TTL of the covered RRset as it appears in the authoritative zone.
        // | SIGNATURE EXPIRATION | a 32-bit value that specifies the expiration date of the signature.
        // | SIGNATURE INCEPTION  | a 32-bit value that specifies the inception date of the signature.
        // | KEY TAG      | a 16-bit value that contains the key tag value of the DNSKEY RR that was used to create the signature.
        // | SIGNER'S NAME| a <domain-name> which specifies the domain name of the signer generating the SIG RR.
        // | SIGNATURE    | a <signature> that authenticates the RRs.
        Ok(Self {
            type_covered: ResourceType(reader.u16()?),
            algorithm: Algorithm(reader.u8()?),
            label_count: reader.u8()?,
            original_ttl: reader.u32()?,
            expiration: reader.u32()?,
            inception: reader.u32()?,
            key_tag: reader.u16()?,
            signers_name: DomainName::parse(reader, ctx)?,
            signature: reader.rest().to_vec(),
        })
    }

    /// Appends the RDATA up to and including the signer's name, which is what RFC 4034, 3.1.8.1 signs over.
    pub fn write_wire_excluding_signature(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        put_u16(out, self.type_covered.0);
        out.push(self.algorithm.0);
        out.push(self.label_count);
        put_u32(out, self.original_ttl);
        put_u32(out, self.expiration);
        put_u32(out, self.inception);
        put_u16(out, self.key_tag);
        self.signers_name.write_wire(out)
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        self.write_wire_excluding_signature(out)?;
        out.extend_from_slice(&self.signature);
        Ok(())
    }
}

impl fmt::Display for Sig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "SIG Type covered: {}, Algorithm: {}, Labels: {}, Original TTL: {}, Signature expiration: {}, \
             Signature inception: {}, Key tag: {}, Signer's name: '{}', Signature: {}",
            self.type_covered,
            self.algorithm,
            self.label_count,
            self.original_ttl,
            UtcTime(self.expiration),
            UtcTime(self.inception),
            self.key_tag,
            self.signers_name,
            Base64(&self.signature)
        )
    }
}

/// RFC 4034, 4.1.2. The Type Bit Maps Field.
pub fn type_bit_maps_from_wire(bytes: &[u8]) -> Result<Vec<ResourceType>, ParseError> {
    type_bit_maps_parse(&mut Reader::new(bytes))
}

fn type_bit_maps_parse(reader: &mut Reader<'_>) -> Result<Vec<ResourceType>, ParseError> {
    // RFC 4034, 4.1.2. The Type Bit Maps Field.
    // Type Bit Maps Field = ( Window Block # | Bitmap Length | Bitmap )+
    // Each bitmap encodes the low-order 8 bits of RR types within the window block, in network bit order.  The first
    // bit is bit 0.  For window block 0, bit 1 corresponds to RR type 1 (A), bit 2 corresponds to RR type 2 (NS), and
    // so forth.
    let mut types = Vec::new();
    let mut previous_window: Option<u8> = None;
    while !reader.is_eof() {
        let window = reader.u8()?;
        let length = reader.u8()?;
        // Blocks are present in the NSEC RR RDATA in increasing numerical order.
        if length == 0 || length > 32 || previous_window.is_some_and(|previous| window <= previous) {
            return Err(ParseError::InvalidTypeBitMap);
        }
        previous_window = Some(window);
        let bitmap = reader.bytes(usize::from(length))?;
        for (index, octet) in (0u16..).zip(bitmap) {
            for bit in 0..8u16 {
                if octet & (0x80 >> bit) != 0 {
                    types.push(ResourceType(u16::from(window) << 8 | (index * 8 + bit)));
                }
            }
        }
    }
    Ok(types)
}

pub fn type_bit_maps_to_wire(types: &[ResourceType], out: &mut Vec<u8>) {
    // Blocks with no types present MUST NOT be included.  Trailing zero octets in the bitmap MUST be omitted.
    let mut bitmaps = [[0u8; 32]; 256];
    let mut lengths = [0usize; 256];
    for rtype in types {
        let [window, low] = rtype.0.to_be_bytes();
        let (window, low) = (usize::from(window), usize::from(low));
        bitmaps[window][low / 8] |= 0x80 >> (low % 8);
        lengths[window] = lengths[window].max(low / 8 + 1);
    }
    for (window, (bitmap, length)) in (0u8..=255).zip(bitmaps.iter().zip(lengths)) {
        if length == 0 {
            continue;
        }
        out.push(window);
        out.push(length as u8);
        out.extend_from_slice(&bitmap[..length]);
    }
}

fn write_types(f: &mut fmt::Formatter<'_>, types: &[ResourceType]) -> fmt::Result {
    f.write_str("Types:")?;
    types.iter().try_for_each(|rtype| write!(f, " {rtype}"))
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Nsec {
    pub next_domain_name: DomainName,
    pub types: Vec<ResourceType>,
}

impl Nsec {
    pub fn has_type(&self, rtype: ResourceType) -> bool {
        self.types.contains(&rtype)
    }

    fn parse(reader: &mut Reader<'_>, ctx: &mut ParseContext) -> Result<Self, ParseError> {
        // RFC 4034, 4.1. NSEC RDATA Wire Format.
        // | Next Domain Name |
        // | Type Bit Maps    |
        Ok(Self {
            next_domain_name: DomainName::parse(reader, ctx)?,
            types: type_bit_maps_parse(reader)?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        self.next_domain_name.write_wire(out)?;
        type_bit_maps_to_wire(&self.types, out);
        Ok(())
    }
}

impl fmt::Display for Nsec {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "NSEC Next: '{}', ", self.next_domain_name)?;
        write_types(f, &self.types)
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Nsec3 {
    pub hash_algorithm: Nsec3HashAlgorithm,
    pub flags: u8,
    pub iterations: u16,
    pub salt: Vec<u8>,
    pub next_hashed_owner_name: Vec<u8>,
    pub types: Vec<ResourceType>,
}

impl Nsec3 {
    // RFC 5155, 3.1.2. Flags: the Opt-Out flag is the least significant bit.
    pub const FLAG_OPT_OUT: u8 = 0b0000_0001;

    pub const fn is_opt_out(&self) -> bool {
        self.flags & Self::FLAG_OPT_OUT != 0
    }

    pub fn has_type(&self, rtype: ResourceType) -> bool {
        self.types.contains(&rtype)
    }

    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 5155, 3.2. NSEC3 RDATA Wire Format.
        // | Hash Alg. | Flags | Iterations |
        // | Salt Length | Salt |
        // | Hash Length | Next Hashed Owner Name |
        // | Type Bit Maps |
        let hash_algorithm = Nsec3HashAlgorithm(reader.u8()?);
        let flags = reader.u8()?;
        let iterations = reader.u16()?;
        let salt = character_string(reader)?;
        let next_hashed_owner_name = character_string(reader)?;
        if next_hashed_owner_name.is_empty() {
            return Err(ParseError::EmptyNextHashedOwnerName);
        }
        Ok(Self {
            hash_algorithm,
            flags,
            iterations,
            salt,
            next_hashed_owner_name,
            types: type_bit_maps_parse(reader)?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        out.push(self.hash_algorithm.0);
        out.push(self.flags);
        put_u16(out, self.iterations);
        put_u8_prefixed(out, &self.salt)?;
        put_u8_prefixed(out, &self.next_hashed_owner_name)?;
        type_bit_maps_to_wire(&self.types, out);
        Ok(())
    }
}

impl fmt::Display for Nsec3 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "NSEC3 Hash: {}, Flags: {}, Iterations: {}, Salt: {}, Next: {}, ",
            self.hash_algorithm,
            self.flags,
            self.iterations,
            HexDump(&self.salt),
            HexDump(&self.next_hashed_owner_name)
        )?;
        write_types(f, &self.types)
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Nsec3Param {
    pub hash_algorithm: Nsec3HashAlgorithm,
    pub flags: u8,
    pub iterations: u16,
    pub salt: Vec<u8>,
}

impl Nsec3Param {
    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 5155, 4.2. NSEC3PARAM RDATA Wire Format.
        // | Hash Alg. | Flags | Iterations |
        // | Salt Length | Salt |
        Ok(Self {
            hash_algorithm: Nsec3HashAlgorithm(reader.u8()?),
            flags: reader.u8()?,
            iterations: reader.u16()?,
            salt: character_string(reader)?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        out.push(self.hash_algorithm.0);
        out.push(self.flags);
        put_u16(out, self.iterations);
        put_u8_prefixed(out, &self.salt)
    }
}

impl fmt::Display for Nsec3Param {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "NSEC3PARAM Hash: {}, Flags: {}, Iterations: {}, Salt: {}",
            self.hash_algorithm,
            self.flags,
            self.iterations,
            HexDump(&self.salt)
        )
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Tlsa {
    pub cert_usage: TlsaCertUsage,
    pub selector: TlsaSelector,
    pub matching_type: TlsaMatchingType,
    pub certificate_association_data: Vec<u8>,
}

impl Tlsa {
    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 6698, 2.1. TLSA RDATA Wire Format.
        // | Cert. Usage | Selector | Matching Type |
        // | Certificate Association Data |
        Ok(Self {
            cert_usage: TlsaCertUsage(reader.u8()?),
            selector: TlsaSelector(reader.u8()?),
            matching_type: TlsaMatchingType(reader.u8()?),
            certificate_association_data: reader.rest().to_vec(),
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) {
        out.push(self.cert_usage.0);
        out.push(self.selector.0);
        out.push(self.matching_type.0);
        out.extend_from_slice(&self.certificate_association_data);
    }
}

impl fmt::Display for Tlsa {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "TLSA Usage: {}, Selector: {}, Matching: {}, Data: {}",
            self.cert_usage.0,
            self.selector.0,
            self.matching_type.0,
            HexDump(&self.certificate_association_data)
        )
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Hinfo {
    pub cpu: Vec<u8>,
    pub os: Vec<u8>,
}

impl Hinfo {
    fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 1035, 3.3.2. HINFO RDATA format.
        // | CPU    | a <character-string> which specifies the CPU type.
        // | OS     | a <character-string> which specifies the operating system type.
        Ok(Self {
            cpu: character_string(reader)?,
            os: character_string(reader)?,
        })
    }

    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        put_u8_prefixed(out, &self.cpu)?;
        put_u8_prefixed(out, &self.os)
    }
}

impl fmt::Display for Hinfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "HINFO CPU: '{}', OS: '{}'",
            String::from_utf8_lossy(&self.cpu),
            String::from_utf8_lossy(&self.os)
        )
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OptOption {
    pub code: u16,
    pub data: Vec<u8>,
}

/// The EDNS(0) OPT pseudo-RR.
//                                   1  1  1  1  1  1
//     0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//    |                UDP Payload Size               |
//    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//    |     Extended RCode    |    VER    |     ZZ    |
//    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//    |DO|                  Z                         |
//    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//    |  OPT-LEN  / OPT-DATA...
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Opt {
    pub udp_payload_size: u16,
    pub extended_rcode_and_flags: u32,
    pub options: Vec<OptOption>,
}

impl Opt {
    pub const MASK_EXTENDED_RCODE: u32 = 0b1111_1111_0000_0000_0000_0000_0000_0000;
    pub const MASK_VERSION: u32 = 0b0000_0000_1111_0000_0000_0000_0000_0000;
    pub const MASK_DO: u32 = 0b0000_0000_0000_0000_1000_0000_0000_0000;

    pub const fn extended_rcode(&self) -> u8 {
        ((self.extended_rcode_and_flags & Self::MASK_EXTENDED_RCODE) >> 24) as u8
    }

    pub const fn version(&self) -> u8 {
        ((self.extended_rcode_and_flags & Self::MASK_VERSION) >> 20) as u8
    }

    pub const fn dnssec_ok(&self) -> bool {
        self.extended_rcode_and_flags & Self::MASK_DO != 0
    }

    pub fn set_version(&mut self, value: u8) {
        self.extended_rcode_and_flags =
            (self.extended_rcode_and_flags & !Self::MASK_VERSION) | ((u32::from(value) << 20) & Self::MASK_VERSION);
    }

    pub fn set_dnssec_ok(&mut self, value: bool) {
        self.extended_rcode_and_flags =
            (self.extended_rcode_and_flags & !Self::MASK_DO) | if value { Self::MASK_DO } else { 0 };
    }

    /// Decodes everything after TYPE.
    pub(crate) fn parse(reader: &mut Reader<'_>) -> Result<Self, ParseError> {
        // RFC 6891, 6.1. The OPT pseudo-RR.
        // This RR does *not* use the standard RDATA format, `reader` starts right after 'TYPE'.
        // | NAME       | empty (root domain)
        // | TYPE       | OPT (41)
        // - we are here -
        // | UDP SIZE   | 16-bit max UDP payload size
        // | RCODE_AND_FLAGS | 32-bit flags and response code
        // | RDLENGTH   | 16-bit length of the RDATA field
        // | RDATA      | variable length, pairs of OPTION-CODE and OPTION-DATA { length(16), data(length) }
        let udp_payload_size = reader.u16()?;
        let extended_rcode_and_flags = reader.u32()?;
        let rd_length = reader.u16()?;
        let mut rdata = reader.sub(usize::from(rd_length))?;
        let mut options = Vec::new();
        while !rdata.is_eof() {
            let code = rdata.u16().map_err(|_| ParseError::InvalidOpt)?;
            let length = rdata.u16().map_err(|_| ParseError::InvalidOpt)?;
            let data = rdata.bytes(usize::from(length)).map_err(|_| ParseError::InvalidOpt)?;
            options.push(OptOption {
                code,
                data: data.to_vec(),
            });
        }
        Ok(Self {
            udp_payload_size,
            extended_rcode_and_flags,
            options,
        })
    }

    /// Appends everything after TYPE: UDP payload size, extended RCODE and flags, RDLENGTH and the options.
    pub fn write_wire(&self, out: &mut Vec<u8>) -> Result<(), EncodeError> {
        put_u16(out, self.udp_payload_size);
        put_u32(out, self.extended_rcode_and_flags);
        let mut rd_length: u16 = 0;
        for option in &self.options {
            let length = u16::try_from(option.data.len()).map_err(|_| EncodeError::TooLarge)?;
            rd_length = rd_length
                .checked_add(4)
                .and_then(|total| total.checked_add(length))
                .ok_or(EncodeError::TooLarge)?;
        }
        put_u16(out, rd_length);
        for option in &self.options {
            put_u16(out, option.code);
            put_u16(out, option.data.len() as u16);
            out.extend_from_slice(&option.data);
        }
        Ok(())
    }
}

impl fmt::Display for Opt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "OPT UDP Payload Size: {}, Extended RCode: {}, Version: {}, DNSSEC OK: {}",
            self.udp_payload_size,
            self.extended_rcode(),
            self.version(),
            self.dnssec_ok()
        )?;
        for option in &self.options {
            write!(f, ", opt[{} = '{}']", option.code, HexDump(&option.data))?;
        }
        Ok(())
    }
}
