/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A small authoritative server for signed zones, enough to drive the validator through every kind of response it
// has to authenticate: positive answers, CNAMEs, wildcards, NXDOMAIN and NODATA with NSEC or NSEC3, and
// delegations with and without DS records.

use std::cmp::Ordering;
use std::collections::HashMap;
use std::net::Ipv4Addr;

use libcrypto_rust::{KeyPair, OkpCurve, ShaHashKind};

use super::crypto::verify_signature;
use super::denial::{Denial, DenialProofs};
use super::rrset::{SignedRRSet, VerifiedRRSet};
use super::{DelegationKind, Fetches, SecurityStatus, Step, ValidationResult, Validator};
use crate::message::{
    Algorithm, Class, DigestType, Dnskey, DomainName, Ds, Message, Nsec, Nsec3, Nsec3HashAlgorithm, Nsec3Param,
    Question, RecordData, ResourceRecord, ResourceType, ResponseCode, Sig, Soa, Tlsa, TlsaCertUsage, TlsaMatchingType,
    TlsaSelector, Txt,
};

pub(crate) const NOW: u32 = 1_800_000_000;

fn name(text: &str) -> DomainName {
    DomainName::from_string(text)
}

fn key_tag_for(key: &Dnskey) -> u16 {
    let mut rdata = Vec::new();
    key.write_wire(&mut rdata);
    let mut sum: u32 = 0;
    for (index, byte) in rdata.iter().enumerate() {
        sum += if index & 1 == 1 {
            u32::from(*byte)
        } else {
            u32::from(*byte) << 8
        };
    }
    sum += (sum >> 16) & 0xffff;
    (sum & 0xffff) as u16
}

fn base32hex_encode(input: &[u8]) -> Vec<u8> {
    const ALPHABET: &[u8; 32] = b"0123456789abcdefghijklmnopqrstuv";
    let mut out = Vec::new();
    let mut bits: u32 = 0;
    let mut bit_count = 0;
    for &byte in input {
        bits = bits << 8 | u32::from(byte);
        bit_count += 8;
        while bit_count >= 5 {
            out.push(ALPHABET[((bits >> (bit_count - 5)) & 0x1f) as usize]);
            bit_count -= 5;
        }
    }
    if bit_count > 0 {
        out.push(ALPHABET[((bits << (5 - bit_count)) & 0x1f) as usize]);
    }
    out
}

fn nsec3_hash(name: &DomainName) -> Vec<u8> {
    let input = name.canonicalized().to_wire().unwrap();
    libcrypto_rust::sha_digest(ShaHashKind::Sha1, &input).unwrap()
}

// The last record in the chain wraps around and covers the names before the first one, too.
fn is_within_interval(after_owner: bool, before_next: bool, wraps: bool) -> bool {
    if wraps {
        after_owner || before_next
    } else {
        after_owner && before_next
    }
}

fn record(owner: &DomainName, rtype: ResourceType, data: RecordData, ttl: u32) -> ResourceRecord {
    ResourceRecord {
        name: owner.clone(),
        rtype,
        class: Class::IN,
        ttl,
        data,
        raw: None,
    }
}

fn soa() -> RecordData {
    RecordData::Soa(Soa {
        mname: name("ns"),
        rname: name("hostmaster"),
        serial: 1,
        refresh: 3600,
        retry: 600,
        expire: 86400,
        minimum: 300,
    })
}

struct Zone {
    apex: DomainName,
    signed: bool,
    use_nsec3: bool,
    nsec3_iterations: u16,
    key_pair: Option<KeyPair>,
    dnskey: Dnskey,
    key_tag: u16,
    // Records keyed by canonical owner name, without further grouping.
    records: HashMap<Vec<u8>, Vec<ResourceRecord>>,
    names: Vec<DomainName>,
    delegations: Vec<DomainName>,
    nsec_records: Vec<ResourceRecord>,
    signing_time: u32,

    omit_denial_records: bool,
    omit_wildcard_proof: bool,
}

impl Zone {
    fn empty(apex: &str) -> Self {
        Self {
            apex: name(apex),
            signed: true,
            use_nsec3: false,
            nsec3_iterations: 0,
            key_pair: None,
            dnskey: Dnskey::default(),
            key_tag: 0,
            records: HashMap::new(),
            names: Vec::new(),
            delegations: Vec::new(),
            nsec_records: Vec::new(),
            signing_time: NOW,
            omit_denial_records: false,
            omit_wildcard_proof: false,
        }
    }

    fn create(apex: &str, use_nsec3: bool) -> Self {
        let mut zone = Self::empty(apex);
        zone.use_nsec3 = use_nsec3;
        let key_pair = libcrypto_rust::okp_generate_key_pair(OkpCurve::Ed25519).unwrap();
        zone.dnskey = Dnskey {
            flags: 257,
            protocol: 3,
            algorithm: Algorithm::ED25519,
            public_key: libcrypto_rust::okp_raw_public_key(&key_pair.public_key).unwrap(),
        };
        zone.key_tag = key_tag_for(&zone.dnskey);
        zone.key_pair = Some(key_pair);
        let apex = zone.apex.clone();
        zone.add(
            &apex,
            ResourceType::DNSKEY,
            RecordData::Dnskey(zone.dnskey.clone()),
            3600,
        );
        zone.add(&apex, ResourceType::SOA, soa(), 3600);
        zone.add(&apex, ResourceType::NS, RecordData::Ns(name("ns")), 3600);
        zone
    }

    fn create_unsigned(apex: &str) -> Self {
        let mut zone = Self::empty(apex);
        zone.signed = false;
        let apex = zone.apex.clone();
        zone.add(&apex, ResourceType::SOA, soa(), 3600);
        zone.add(&apex, ResourceType::NS, RecordData::Ns(name("ns")), 3600);
        zone
    }

    fn ds(&self) -> Ds {
        let mut input = self.apex.canonicalized().to_wire().unwrap();
        self.dnskey.write_wire(&mut input);
        Ds {
            key_tag: self.key_tag,
            algorithm: Algorithm::ED25519,
            digest_type: DigestType::SHA256,
            digest: libcrypto_rust::sha_digest(ShaHashKind::Sha256, &input).unwrap(),
        }
    }

    fn add(&mut self, owner: &DomainName, rtype: ResourceType, data: RecordData, ttl: u32) {
        let key = owner.to_canonical_string();
        if !self.records.contains_key(&key) {
            self.names.push(owner.clone());
        }
        self.records
            .entry(key)
            .or_default()
            .push(record(owner, rtype, data, ttl));
    }

    fn add_a(&mut self, owner: &str, address: Ipv4Addr) {
        self.add(&name(owner), ResourceType::A, RecordData::A(address), 300);
    }

    fn add_cname(&mut self, owner: &str, target: &str) {
        self.add(&name(owner), ResourceType::CNAME, RecordData::Cname(name(target)), 300);
    }

    fn add_dname(&mut self, owner: &str, target: &str, ttl: u32) {
        self.add(&name(owner), ResourceType::DNAME, RecordData::Dname(name(target)), ttl);
    }

    // The owner of a DNAME above `name`, which redirects it.
    fn dname_above(&self, name: &DomainName) -> Option<DomainName> {
        (self.apex.labels.len()..name.labels.len())
            .map(|labels| name.suffix(labels))
            .find(|owner| !self.rrset(owner, ResourceType::DNAME).is_empty())
    }

    // A delegation: the NS record at the child's apex, and a DS record for a signed child.
    fn delegate(&mut self, child: &Zone) {
        self.delegate_with_ds(child, child.signed.then(|| child.ds()));
    }

    fn delegate_with_ds(&mut self, child: &Zone, ds: Option<Ds>) {
        self.delegations.push(child.apex.clone());
        self.add(&child.apex, ResourceType::NS, RecordData::Ns(name("ns")), 3600);
        if let Some(ds) = ds {
            self.add(&child.apex, ResourceType::DS, RecordData::Ds(ds), 3600);
        }
    }

    fn types_at(&self, name: &DomainName) -> Vec<ResourceType> {
        let mut types = Vec::new();
        // RFC 5155, 7.1: an empty non-terminal's NSEC3 RR has an empty type bit map.
        let Some(records) = self.records.get(&name.to_canonical_string()) else {
            return types;
        };
        for record in records {
            if !types.contains(&record.rtype) {
                types.push(record.rtype);
            }
        }
        types.push(ResourceType::RRSIG);
        // RFC 5155, 3.1.8. Type Bit Maps.
        // The Type Bit Maps field identifies the RRSet types that exist at the original owner name of the NSEC3 RR.
        // The NSEC3 RR itself lives at the hashed owner name, so only NSEC is listed here.
        if !self.use_nsec3 {
            types.push(ResourceType::NSEC);
        }
        types
    }

    // Generates the NSEC or NSEC3 chain. Call after every record has been added.
    fn finalize(&mut self) {
        if !self.signed {
            return;
        }

        let mut sorted_names = self.names.clone();
        if self.use_nsec3 {
            // Each empty non-terminal MUST have a corresponding NSEC3 RR.
            sorted_names.extend(self.empty_non_terminals());
            sorted_names.sort_by_key(nsec3_hash);
            let apex = self.apex.clone();
            self.add(
                &apex,
                ResourceType::NSEC3PARAM,
                RecordData::Nsec3Param(Nsec3Param {
                    hash_algorithm: Nsec3HashAlgorithm::SHA1,
                    flags: 0,
                    iterations: self.nsec3_iterations,
                    salt: Vec::new(),
                }),
                0,
            );
            for (index, name) in sorted_names.iter().enumerate() {
                let next = nsec3_hash(&sorted_names[(index + 1) % sorted_names.len()]);
                let owner = self.apex.with_label_prepended(base32hex_encode(&nsec3_hash(name)));
                let types = self.types_at(name);
                let nsec3 = Nsec3 {
                    hash_algorithm: Nsec3HashAlgorithm::SHA1,
                    flags: 0,
                    iterations: self.nsec3_iterations,
                    salt: Vec::new(),
                    next_hashed_owner_name: next,
                    types,
                };
                self.nsec_records
                    .push(record(&owner, ResourceType::NSEC3, RecordData::Nsec3(nsec3), 300));
            }
            return;
        }

        sorted_names.sort_by(DomainName::canonical_cmp);
        for (index, name) in sorted_names.iter().enumerate() {
            let next = sorted_names[(index + 1) % sorted_names.len()].clone();
            let nsec = Nsec {
                next_domain_name: next,
                types: self.types_at(name),
            };
            self.nsec_records
                .push(record(name, ResourceType::NSEC, RecordData::Nsec(nsec), 300));
        }
    }

    fn sign(&self, rrset: &[ResourceRecord], signature_owner: &DomainName) -> ResourceRecord {
        let owner = signature_owner.canonicalized();
        let first = &rrset[0];
        let mut rrsig = Sig {
            type_covered: first.rtype,
            algorithm: Algorithm::ED25519,
            label_count: if owner.is_wildcard() {
                owner.labels.len() - 1
            } else {
                owner.labels.len()
            } as u8,
            original_ttl: first.ttl,
            expiration: self.signing_time.wrapping_add(3600),
            inception: self.signing_time.wrapping_sub(3600),
            key_tag: self.key_tag,
            signers_name: self.apex.clone(),
            signature: Vec::new(),
        };

        let rdata_offset = owner.to_wire().unwrap().len() + 2 + 2 + 4 + 2;
        let mut encoded: Vec<Vec<u8>> = rrset
            .iter()
            .map(|record| {
                let mut buffer = Vec::new();
                record
                    .write_canonical_wire(&mut buffer, &owner, rrsig.original_ttl)
                    .unwrap();
                buffer
            })
            .collect();
        encoded.sort_by(|a, b| a[rdata_offset..].cmp(&b[rdata_offset..]));

        let mut signed_data = Vec::new();
        rrsig.write_wire_excluding_signature(&mut signed_data).unwrap();
        for buffer in &encoded {
            signed_data.extend_from_slice(buffer);
        }

        let private_key = &self.key_pair.as_ref().unwrap().private_key;
        rrsig.signature = libcrypto_rust::okp_sign(private_key, &signed_data, &[]).unwrap();
        record(&first.name, ResourceType::RRSIG, RecordData::Rrsig(rrsig), first.ttl)
    }

    fn rrset(&self, owner: &DomainName, rtype: ResourceType) -> Vec<ResourceRecord> {
        self.records
            .get(&owner.to_canonical_string())
            .map(|records| records.iter().filter(|record| record.rtype == rtype).cloned().collect())
            .unwrap_or_default()
    }

    fn has_name(&self, name: &DomainName) -> bool {
        self.records.contains_key(&name.to_canonical_string())
    }

    fn is_empty_non_terminal(&self, name: &DomainName) -> bool {
        !self.has_name(name)
            && self
                .names
                .iter()
                .any(|owner| name.is_ancestor_or_equal_of(owner) && !name.eq_ignore_case(owner))
    }

    fn empty_non_terminals(&self) -> Vec<DomainName> {
        let mut result: Vec<DomainName> = Vec::new();
        for owner in &self.names {
            let mut ancestor = owner.parent();
            while ancestor.labels.len() > self.apex.labels.len() {
                if self.is_empty_non_terminal(&ancestor) && !result.iter().any(|known| known.eq_ignore_case(&ancestor))
                {
                    result.push(ancestor.clone());
                }
                ancestor = ancestor.parent();
            }
        }
        result
    }

    fn exists(&self, name: &DomainName) -> bool {
        self.has_name(name) || self.is_empty_non_terminal(name)
    }

    // Appends an RRset and its signature to a section, optionally renaming the records (wildcard expansion or
    // a spoofed owner) while signing as `signature_owner`.
    fn append_signed(
        &self,
        section: &mut Vec<ResourceRecord>,
        mut rrset: Vec<ResourceRecord>,
        signature_owner: &DomainName,
        presented_owner: Option<&DomainName>,
    ) {
        if rrset.is_empty() {
            return;
        }
        let mut signature = self.signed.then(|| self.sign(&rrset, signature_owner));
        if let Some(presented_owner) = presented_owner {
            for record in &mut rrset {
                record.name = presented_owner.clone();
            }
            if let Some(signature) = &mut signature {
                signature.name = presented_owner.clone();
            }
        }
        section.extend(rrset);
        section.extend(signature);
    }

    fn nsec_matching(&self, name: &DomainName) -> Vec<ResourceRecord> {
        let owner = if self.use_nsec3 {
            self.apex.with_label_prepended(base32hex_encode(&nsec3_hash(name)))
        } else {
            name.clone()
        };
        self.nsec_records
            .iter()
            .filter(|record| record.name.eq_ignore_case(&owner))
            .cloned()
            .collect()
    }

    fn nsec_covering(&self, name: &DomainName) -> Vec<ResourceRecord> {
        let mut result = Vec::new();
        if self.use_nsec3 {
            let hash = nsec3_hash(name);
            for record in &self.nsec_records {
                let RecordData::Nsec3(nsec3) = &record.data else {
                    unreachable!();
                };
                // Recover the owner hash from the record's owner label.
                let mut owner_hash = nsec3_hash(&DomainName::root());
                for candidate in self.names.iter().chain(&self.empty_non_terminals()) {
                    if record
                        .name
                        .eq_ignore_case(&self.apex.with_label_prepended(base32hex_encode(&nsec3_hash(candidate))))
                    {
                        owner_hash = nsec3_hash(candidate);
                        break;
                    }
                }
                let after_owner = owner_hash < hash;
                let before_next = hash < nsec3.next_hashed_owner_name;
                let wraps = nsec3.next_hashed_owner_name <= owner_hash;
                if is_within_interval(after_owner, before_next, wraps) {
                    result.push(record.clone());
                }
            }
            return result;
        }
        for record in &self.nsec_records {
            let RecordData::Nsec(nsec) = &record.data else {
                unreachable!();
            };
            let after_owner = record.name.canonical_cmp(name) == Ordering::Less;
            let before_next = name.canonical_cmp(&nsec.next_domain_name) == Ordering::Less;
            let wraps = nsec.next_domain_name.canonical_cmp(&record.name) != Ordering::Greater;
            if is_within_interval(after_owner, before_next, wraps) {
                result.push(record.clone());
            }
        }
        result
    }

    fn closest_encloser(&self, name: &DomainName) -> DomainName {
        let mut candidate = name.clone();
        while !self.exists(&candidate) && candidate.labels.len() > self.apex.labels.len() {
            candidate = candidate.parent();
        }
        candidate
    }

    fn append_denial(&self, authority: &mut Vec<ResourceRecord>, mut denial_records: Vec<ResourceRecord>) {
        if self.omit_denial_records {
            return;
        }
        // Group by owner so each NSEC RRset gets one signature.
        while let Some(first) = denial_records.first() {
            let owner = first.name.clone();
            let (group, rest): (Vec<_>, Vec<_>) = denial_records
                .into_iter()
                .partition(|record| record.name.eq_ignore_case(&owner));
            denial_records = rest;
            let already_present = authority
                .iter()
                .any(|record| record.rtype == group[0].rtype && record.name.eq_ignore_case(&owner));
            if !already_present {
                self.append_signed(authority, group, &owner, None);
            }
        }
    }

    fn respond(&self, question: &Question) -> Message {
        let mut response = Message::default();
        response.header.options.set_is_question(false);
        response.header.options.set_authenticated_data(true);
        response.questions.push(question.clone());

        let mut name = question.name.clone();
        let mut answers = Vec::new();
        let mut authority = Vec::new();

        'done: {
            for _ in 0..8 {
                // A delegated name (or anything below it): a referral with the DS or a proof there is none.
                for delegation in &self.delegations {
                    if delegation.is_ancestor_or_equal_of(&name)
                        && !(question.rtype == ResourceType::DS && delegation.eq_ignore_case(&name))
                    {
                        self.append_signed(
                            &mut authority,
                            self.rrset(delegation, ResourceType::DS),
                            delegation,
                            None,
                        );
                        authority.extend(self.rrset(delegation, ResourceType::NS));
                        if self.rrset(delegation, ResourceType::DS).is_empty() {
                            self.append_denial(&mut authority, self.nsec_matching(delegation));
                        }
                        break 'done;
                    }
                }

                // Below a DNAME: the DNAME, and a CNAME synthesized from it, which is not signed.
                if let Some(owner) = self.dname_above(&name) {
                    let dname = self.rrset(&owner, ResourceType::DNAME);
                    let RecordData::Dname(dname_target) = dname[0].data.clone() else {
                        unreachable!();
                    };
                    let ttl = dname[0].ttl;
                    self.append_signed(&mut answers, dname, &owner, None);
                    let mut target = DomainName {
                        labels: name.labels[..name.labels.len() - owner.labels.len()].to_vec(),
                    };
                    target.labels.extend(dname_target.labels);
                    answers.push(record(
                        &name,
                        ResourceType::CNAME,
                        RecordData::Cname(target.clone()),
                        ttl,
                    ));
                    if question.rtype == ResourceType::CNAME || !self.apex.is_ancestor_or_equal_of(&target) {
                        break 'done;
                    }
                    name = target;
                    continue;
                }

                if self.exists(&name) {
                    let exact = self.rrset(&name, question.rtype);
                    if !exact.is_empty() {
                        self.append_signed(&mut answers, exact, &name, None);
                        break 'done;
                    }
                    let cname = self.rrset(&name, ResourceType::CNAME);
                    if !cname.is_empty() && question.rtype != ResourceType::CNAME {
                        let RecordData::Cname(target) = cname[0].data.clone() else {
                            unreachable!();
                        };
                        self.append_signed(&mut answers, cname, &name, None);
                        if !self.apex.is_ancestor_or_equal_of(&target) {
                            break 'done;
                        }
                        name = target;
                        continue;
                    }
                    // No Data. An empty non-terminal has no NSEC, so the one covering it stands in.
                    self.append_signed(
                        &mut authority,
                        self.rrset(&self.apex, ResourceType::SOA),
                        &self.apex,
                        None,
                    );
                    if self.use_nsec3 || self.has_name(&name) {
                        self.append_denial(&mut authority, self.nsec_matching(&name));
                    } else {
                        self.append_denial(&mut authority, self.nsec_covering(&name));
                    }
                    break 'done;
                }

                // Wildcard expansion, or Name Error.
                let encloser = self.closest_encloser(&name);
                let wildcard = encloser.with_label_prepended("*");
                let next_closer = name.suffix(encloser.labels.len() + 1);
                if self.has_name(&wildcard) && !self.rrset(&wildcard, question.rtype).is_empty() {
                    self.append_signed(
                        &mut answers,
                        self.rrset(&wildcard, question.rtype),
                        &wildcard,
                        Some(&name),
                    );
                    if !self.omit_wildcard_proof {
                        self.append_denial(&mut authority, self.nsec_covering(&next_closer));
                    }
                    break 'done;
                }
                if self.has_name(&wildcard) {
                    // Wildcard No Data.
                    self.append_signed(
                        &mut authority,
                        self.rrset(&self.apex, ResourceType::SOA),
                        &self.apex,
                        None,
                    );
                    if self.use_nsec3 {
                        self.append_denial(&mut authority, self.nsec_matching(&encloser));
                    }
                    self.append_denial(&mut authority, self.nsec_covering(&next_closer));
                    self.append_denial(&mut authority, self.nsec_matching(&wildcard));
                    break 'done;
                }

                response.header.options.set_response_code(ResponseCode::NAME_ERROR);
                self.append_signed(
                    &mut authority,
                    self.rrset(&self.apex, ResourceType::SOA),
                    &self.apex,
                    None,
                );
                if self.use_nsec3 {
                    self.append_denial(&mut authority, self.nsec_matching(&encloser));
                    self.append_denial(&mut authority, self.nsec_covering(&next_closer));
                } else {
                    self.append_denial(&mut authority, self.nsec_covering(&name));
                }
                self.append_denial(&mut authority, self.nsec_covering(&wildcard));
                break 'done;
            }
        }

        response.answers = answers;
        response.authorities = authority;
        response
    }
}

pub(crate) struct Server {
    zones: Vec<Zone>,
    queries: usize,
}

impl Server {
    fn zone_for(&self, question: &Question) -> &Zone {
        self.zones
            .iter()
            .filter(|zone| zone.apex.is_ancestor_or_equal_of(&question.name))
            // DS records live in the parent zone.
            .filter(|zone| {
                !(question.rtype == ResourceType::DS
                    && zone.apex.eq_ignore_case(&question.name)
                    && !zone.apex.is_root())
            })
            .max_by_key(|zone| zone.apex.labels.len())
            .unwrap()
    }

    pub(crate) fn start(&mut self) {
        for zone in &mut self.zones {
            zone.finalize();
        }
    }

    // The response goes through the wire format, as it would over the network.
    pub(crate) fn answer(&mut self, question: &Question) -> Message {
        self.queries += 1;
        let response = self.zone_for(question).respond(question);
        Message::from_wire(&response.to_wire().unwrap()).unwrap()
    }

    pub(crate) fn root_ds(&self) -> Ds {
        self.zones[0].ds()
    }

    /// Makes the signatures of "example" expire in `seconds` from `NOW`. Call it before `start()`.
    pub(crate) fn expire_example_signatures_in(&mut self, seconds: u32) {
        self.zones[1].signing_time = NOW + seconds - 3600;
    }

    fn anchor(&self, validator: &mut Validator) {
        validator.set_trust_anchors(vec![self.zones[0].ds()]);
    }
}

// Root zone, a signed child "example" with an address, a CNAME, a wildcard, and an unsigned grandchild.
pub(crate) fn make_server(use_nsec3: bool) -> Server {
    let mut root = Zone::create("", use_nsec3);
    let mut example = Zone::create("example", use_nsec3);
    let mut unsigned_child = Zone::create_unsigned("unsigned.example");

    example.add_a("www.example", Ipv4Addr::new(192, 0, 2, 1));
    example.add_a("other.example", Ipv4Addr::new(192, 0, 2, 2));
    example.add_cname("alias.example", "www.example");
    example.add_a("*.wild.example", Ipv4Addr::new(192, 0, 2, 3));
    example.add(
        &name("wild.example"),
        ResourceType::TXT,
        RecordData::Txt(Txt {
            strings: vec![b"wild".to_vec()],
        }),
        300,
    );
    unsigned_child.add_a("host.unsigned.example", Ipv4Addr::new(192, 0, 2, 4));

    example.delegate(&unsigned_child);
    root.delegate(&example);

    Server {
        zones: vec![root, example, unsigned_child],
        queries: 0,
    }
}

/// One validated lookup, driven by hand: the validator asks for what it needs, and the server answers.
struct Lookup {
    question: Question,
    response: Message,
    fetches: Fetches,
}

impl Lookup {
    fn start(server: &mut Server, name: &str, rtype: ResourceType) -> Self {
        let question = Question {
            name: DomainName::from_string(name),
            rtype,
            class: Class::IN,
        };
        let response = server.answer(&question);
        Self {
            question,
            response,
            fetches: Fetches::new(),
        }
    }

    fn step(&self, validator: &mut Validator, now: u32) -> Step {
        validator.validate(&self.response, &self.question, &self.fetches, now)
    }

    fn fetch(&mut self, server: &mut Server, question: &Question) {
        assert!(self.fetches.get(&question.name, question.rtype).is_none());
        let response = server.answer(question);
        self.fetches.insert(question, response);
    }

    // As a resolver does it: a Bogus result fails the lookup.
    fn finish(
        mut self,
        server: &mut Server,
        validator: &mut Validator,
        now: u32,
    ) -> Result<ValidationResult, &'static str> {
        for _ in 0..64 {
            match self.step(validator, now) {
                Step::Done(result) if result.status == SecurityStatus::Bogus => return Err(result.reason),
                Step::Done(result) => return Ok(result),
                Step::Failed(reason) => return Err(reason),
                Step::NeedFetch(question) => self.fetch(server, &question),
            }
        }
        panic!("validation did not finish");
    }
}

fn validated_lookup_at(
    server: &mut Server,
    validator: &mut Validator,
    name: &str,
    rtype: ResourceType,
    now: u32,
) -> Result<ValidationResult, &'static str> {
    Lookup::start(server, name, rtype).finish(server, validator, now)
}

fn validated_lookup(
    server: &mut Server,
    validator: &mut Validator,
    name: &str,
    rtype: ResourceType,
) -> Result<ValidationResult, &'static str> {
    validated_lookup_at(server, validator, name, rtype, NOW)
}

fn first_address(result: &ValidationResult) -> Option<Ipv4Addr> {
    result.records.iter().find_map(|record| match record.data {
        RecordData::A(address) => Some(address),
        _ => None,
    })
}

fn setup(use_nsec3: bool, configure: impl FnOnce(&mut Server)) -> (Server, Validator) {
    let mut server = make_server(use_nsec3);
    configure(&mut server);
    server.start();
    let mut validator = Validator::new();
    server.anchor(&mut validator);
    (server, validator)
}

#[test]
fn secure_answer_validates() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 1)));

        // The chain is cached: a second name in the zone needs only its own query and a DS probe at that name.
        let queries_before = server.queries;
        let other = validated_lookup(&mut server, &mut validator, "other.example", ResourceType::A).unwrap();
        assert_eq!(other.status, SecurityStatus::Secure);
        assert_eq!(first_address(&other), Some(Ipv4Addr::new(192, 0, 2, 2)));
        assert_eq!(server.queries - queries_before, 2);
    }
}

#[test]
fn cname_chain_validates() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "alias.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 1)));
    }
}

#[test]
fn cname_chain_caps_ttls() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |server| {
            let zone = &mut server.zones[1];
            zone.add(
                &name("first.example"),
                ResourceType::CNAME,
                RecordData::Cname(name("second.example")),
                100,
            );
            zone.add(
                &name("second.example"),
                ResourceType::CNAME,
                RecordData::Cname(name("www.example")),
                250,
            );
        });

        let result = validated_lookup(&mut server, &mut validator, "first.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        let ttls: Vec<_> = result
            .records
            .iter()
            .map(|record| (record.name.to_string(), record.rtype, record.ttl))
            .collect();
        assert_eq!(
            ttls,
            [
                ("first.example.".to_string(), ResourceType::CNAME, 100),
                ("second.example.".to_string(), ResourceType::CNAME, 100),
                ("www.example.".to_string(), ResourceType::A, 100),
            ]
        );
    }
}

#[test]
fn cname_to_an_unsigned_zone_keeps_its_validated_ttl() {
    // The signatures in "example" expire in 50 seconds.
    let (mut server, mut validator) = setup(false, |server| {
        let zone = &mut server.zones[1];
        zone.signing_time = NOW - 3550;
        zone.add_cname("first.example", "host.unsigned.example");
    });

    // The rest of the chain, as a recursive resolver would include it.
    let mut lookup = Lookup::start(&mut server, "first.example", ResourceType::A);
    lookup
        .response
        .answers
        .extend(server.zones[2].rrset(&name("host.unsigned.example"), ResourceType::A));

    let result = lookup.finish(&mut server, &mut validator, NOW).unwrap();
    assert_eq!(result.status, SecurityStatus::Insecure);
    let ttls: Vec<_> = result
        .records
        .iter()
        .map(|record| (record.name.to_string(), record.rtype, record.ttl))
        .collect();
    assert_eq!(
        ttls,
        [
            ("first.example.".to_string(), ResourceType::CNAME, 50),
            ("host.unsigned.example.".to_string(), ResourceType::A, 50),
        ]
    );
}

#[test]
fn wildcard_answer_validates_with_proof() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "anything.wild.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 3)));
    }
}

#[test]
fn wildcard_answer_lives_no_longer_than_its_proof() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});
        for record in &mut server.zones[1].nsec_records {
            record.ttl = 5;
        }

        let result = validated_lookup(&mut server, &mut validator, "anything.wild.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(result.records.len(), 1);
        assert_eq!(result.records[0].ttl, 5);

        // An exact answer does not depend on the proof.
        let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
        assert_eq!(result.records[0].ttl, 300);
    }
}

#[test]
fn wildcard_answer_without_proof_is_bogus() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |server| server.zones[1].omit_wildcard_proof = true);

        assert!(validated_lookup(&mut server, &mut validator, "anything.wild.example", ResourceType::A).is_err());
    }
}

#[test]
fn wildcard_answer_for_a_name_below_an_empty_non_terminal_is_bogus() {
    // "y.wild.example" owns no records, but exists as the parent of "x.y.wild.example", so "*.wild.example" does not
    // answer for "a.y.wild.example".
    let (mut server, mut validator) = setup(false, |server| {
        server.zones[1].add_a("x.y.wild.example", Ipv4Addr::new(192, 0, 2, 9));
    });

    // The NSEC from "*.wild.example" to "x.y.wild.example" spans "y.wild.example".
    let mut lookup = Lookup::start(&mut server, "a.y.wild.example", ResourceType::A);
    let zone = &server.zones[1];
    let wildcard = name("*.wild.example");
    lookup.response.header.options.set_response_code(ResponseCode::NO_ERROR);
    lookup.response.answers.clear();
    lookup.response.authorities.clear();
    zone.append_signed(
        &mut lookup.response.answers,
        zone.rrset(&wildcard, ResourceType::A),
        &wildcard,
        Some(&name("a.y.wild.example")),
    );
    zone.append_denial(&mut lookup.response.authorities, zone.nsec_matching(&wildcard));

    assert_eq!(
        lookup.finish(&mut server, &mut validator, NOW),
        Err("Wildcard expansion is not proven")
    );
}

#[test]
fn name_error_is_proven() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "missing.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert!(result.records.is_empty());
    }
}

#[test]
fn negative_answers_carry_the_lifetime_of_their_proof() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});
        let result = validated_lookup(&mut server, &mut validator, "missing.example", ResourceType::A).unwrap();
        assert_eq!(result.denial_ttl, Some(300));
        let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
        assert_eq!(result.denial_ttl, None);

        // The signatures in "example" expire in a second.
        let (mut server, mut validator) = setup(use_nsec3, |server| server.zones[1].signing_time = NOW - 3599);
        let result = validated_lookup(&mut server, &mut validator, "missing.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(result.denial_ttl, Some(1));
        let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::AAAA).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(result.denial_ttl, Some(1));
    }
}

#[test]
fn name_error_below_the_first_nsec3_hash_is_proven() {
    let (mut server, mut validator) = setup(true, |_| {});

    // A name whose hash sorts before every hashed owner name in the zone is covered by the last NSEC3 in hash
    // order, whose next hashed owner name wraps around to the first.
    let zone = &server.zones[1];
    let first_hash = zone.names.iter().map(nsec3_hash).min().unwrap();
    let missing = (0..)
        .map(|index| name(&format!("n{index}.example")))
        .find(|candidate| nsec3_hash(candidate) < first_hash)
        .unwrap();
    let last = zone
        .nsec_records
        .iter()
        .find(|record| matches!(&record.data, RecordData::Nsec3(nsec3) if nsec3.next_hashed_owner_name == first_hash))
        .unwrap()
        .clone();
    let lookup = Lookup::start(&mut server, &missing.to_string(), ResourceType::A);
    assert!(
        lookup
            .response
            .authorities
            .iter()
            .any(|record| record.rtype == ResourceType::NSEC3 && record.name.eq_ignore_case(&last.name))
    );
    let result = lookup.finish(&mut server, &mut validator, NOW).unwrap();
    assert_eq!(result.status, SecurityStatus::Secure);
    assert!(result.records.is_empty());
}

// Drops the NSEC and NSEC3 records, and their signatures, from a response.
fn strip_denial_records(response: &mut Message) {
    let is_denial = |rtype: ResourceType| rtype == ResourceType::NSEC || rtype == ResourceType::NSEC3;
    response.authorities.retain(|record| match &record.data {
        RecordData::Rrsig(rrsig) => !is_denial(rrsig.type_covered),
        _ => !is_denial(record.rtype),
    });
}

#[test]
fn name_error_without_proof_is_bogus() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let mut lookup = Lookup::start(&mut server, "missing.example", ResourceType::A);
        strip_denial_records(&mut lookup.response);
        assert_eq!(
            lookup.finish(&mut server, &mut validator, NOW),
            Err("Name error is not proven")
        );
    }
}

#[test]
fn no_data_is_proven() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::AAAA).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert!(result.records.is_empty());
    }
}

// "_tcp.example" owns no records, but exists as the parent of "_443._tcp.example".
fn add_tlsa_below_empty_non_terminal(server: &mut Server) {
    server.zones[1].add(
        &name("_443._tcp.example"),
        ResourceType::TLSA,
        RecordData::Tlsa(Tlsa {
            cert_usage: TlsaCertUsage(3),
            selector: TlsaSelector(1),
            matching_type: TlsaMatchingType(1),
            certificate_association_data: vec![0xab; 32],
        }),
        300,
    );
}

#[test]
fn no_data_at_empty_non_terminal_is_proven() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, add_tlsa_below_empty_non_terminal);

        let result = validated_lookup(&mut server, &mut validator, "_tcp.example", ResourceType::TLSA).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert!(result.records.is_empty());

        let result = validated_lookup(&mut server, &mut validator, "_443._tcp.example", ResourceType::TLSA).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(result.records.len(), 1);
    }
}

#[test]
fn name_error_at_empty_non_terminal_is_bogus() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, add_tlsa_below_empty_non_terminal);

        // The No Data response, passed off as a Name Error.
        let mut lookup = Lookup::start(&mut server, "_tcp.example", ResourceType::TLSA);
        lookup
            .response
            .header
            .options
            .set_response_code(ResponseCode::NAME_ERROR);
        assert_eq!(
            lookup.finish(&mut server, &mut validator, NOW),
            Err("Name error is not proven")
        );
    }
}

#[test]
fn nsec3_iterations_count_only_for_usable_records() {
    for (hash_algorithm, expected) in [
        (Nsec3HashAlgorithm(2), SecurityStatus::Secure),
        (Nsec3HashAlgorithm::SHA1, SecurityStatus::Insecure),
    ] {
        let (mut server, mut validator) = setup(true, |_| {});

        // A signed NSEC3 with 101 iterations, next to the proof.
        let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::AAAA);
        let zone = &server.zones[1];
        let owner = zone
            .apex
            .with_label_prepended(base32hex_encode(&nsec3_hash(&name("unrelated.example"))));
        let nsec3 = Nsec3 {
            hash_algorithm,
            flags: 0,
            iterations: 101,
            salt: Vec::new(),
            next_hashed_owner_name: vec![0xff; 20],
            types: vec![ResourceType::A],
        };
        let extra = vec![record(&owner, ResourceType::NSEC3, RecordData::Nsec3(nsec3), 300)];
        zone.append_signed(&mut lookup.response.authorities, extra, &owner, None);

        let result = lookup.finish(&mut server, &mut validator, NOW).unwrap();
        assert_eq!(result.status, expected);
        assert!(result.records.is_empty());
    }
}

#[test]
fn nsec3_iterations_do_not_excuse_a_bad_answer_signature() {
    let (mut server, mut validator) = setup(true, |_| {});

    // A signed NSEC3 with 101 iterations next to an answer with a broken signature.
    let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::A);
    for record in &mut lookup.response.answers {
        if let RecordData::Rrsig(rrsig) = &mut record.data {
            rrsig.signature[0] ^= 0x01;
        }
    }
    let zone = &server.zones[1];
    let owner = zone
        .apex
        .with_label_prepended(base32hex_encode(&nsec3_hash(&name("unrelated.example"))));
    let nsec3 = Nsec3 {
        hash_algorithm: Nsec3HashAlgorithm::SHA1,
        flags: 0,
        iterations: 101,
        salt: Vec::new(),
        next_hashed_owner_name: vec![0xff; 20],
        types: vec![ResourceType::A],
    };
    let extra = vec![record(&owner, ResourceType::NSEC3, RecordData::Nsec3(nsec3), 300)];
    zone.append_signed(&mut lookup.response.authorities, extra, &owner, None);

    assert_eq!(
        lookup.finish(&mut server, &mut validator, NOW),
        Err("RRset signature did not validate")
    );
}

#[test]
fn no_data_without_proof_is_bogus() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::AAAA);
        strip_denial_records(&mut lookup.response);
        assert_eq!(
            lookup.finish(&mut server, &mut validator, NOW),
            Err("Absence of the requested type is not proven")
        );
    }
}

#[test]
fn bad_signature_is_bogus() {
    let (mut server, mut validator) = setup(false, |_| {});

    let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::A);
    for record in &mut lookup.response.answers {
        if let RecordData::Rrsig(rrsig) = &mut record.data {
            rrsig.signature[0] ^= 0x01;
        }
    }
    assert_eq!(
        lookup.finish(&mut server, &mut validator, NOW),
        Err("RRset signature did not validate")
    );
}

#[test]
fn ttl_is_capped_by_the_rrsig_that_validates() {
    let (mut server, mut validator) = setup(false, |_| {});

    // An expired RRSIG with a TTL of 0 next to the valid one.
    let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::A);
    let mut expired = lookup
        .response
        .answers
        .iter()
        .find(|record| record.rtype == ResourceType::RRSIG)
        .unwrap()
        .clone();
    expired.ttl = 0;
    if let RecordData::Rrsig(rrsig) = &mut expired.data {
        rrsig.expiration = NOW - 1;
    }
    lookup.response.answers.insert(0, expired);

    let result = lookup.finish(&mut server, &mut validator, NOW).unwrap();
    assert_eq!(result.status, SecurityStatus::Secure);
    assert_eq!(result.records.len(), 1);
    assert_eq!(result.records[0].ttl, 300);
}

#[test]
fn dname_target_is_canonicalized_for_verification() {
    let (mut server, mut validator) = setup(false, |server| {
        server.zones[1].add(
            &name("old.example"),
            ResourceType::DNAME,
            RecordData::Dname(name("Target.Example")),
            300,
        );
    });

    // RFC 4034, 6.2: the signature covers the target in lowercase.
    let mut lookup = Lookup::start(&mut server, "old.example", ResourceType::DNAME);
    let zone = &server.zones[1];
    let owner = name("old.example");
    let mut canonical = zone.rrset(&owner, ResourceType::DNAME);
    for record in &mut canonical {
        record.data = RecordData::Dname(name("target.example"));
    }
    lookup.response.answers = zone.rrset(&owner, ResourceType::DNAME);
    lookup.response.answers.push(zone.sign(&canonical, &owner));

    let result = lookup.finish(&mut server, &mut validator, NOW).unwrap();
    assert_eq!(result.status, SecurityStatus::Secure);
    assert_eq!(result.records.len(), 1);
    assert_eq!(result.records[0].data, RecordData::Dname(name("Target.Example")));
}

#[test]
fn names_with_dots_in_labels_are_kept_apart() {
    let one_label = DomainName {
        labels: vec![b"www.example".to_vec()],
    };
    let question = |name: &DomainName| Question {
        name: name.clone(),
        rtype: ResourceType::DS,
        class: Class::IN,
    };

    let mut fetches = Fetches::new();
    fetches.insert(&question(&one_label), Message::default());
    assert!(fetches.get(&name("www.example"), ResourceType::DS).is_none());
    let upper_case = DomainName {
        labels: vec![b"WWW.example".to_vec()],
    };
    assert!(fetches.get(&upper_case, ResourceType::DS).is_some());

    // Once "www.example" is known not to be a zone cut, the one-label name still needs its own DS query.
    let (mut server, mut validator) = setup(false, |_| {});
    validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    let address_question = Question {
        rtype: ResourceType::A,
        ..question(&one_label)
    };
    let mut lookup = Lookup {
        response: server.answer(&address_question),
        question: address_question,
        fetches: Fetches::new(),
    };
    let mut asked_for_ds = false;
    while let Step::NeedFetch(fetch) = lookup.step(&mut validator, NOW) {
        asked_for_ds |= fetch == question(&one_label);
        lookup.fetch(&mut server, &fetch);
    }
    assert!(asked_for_ds);
}

#[test]
fn answer_for_another_name_is_bogus() {
    let (mut server, mut validator) = setup(false, |_| {});

    // The signed RRset for other.example, presented under the queried name.
    let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::A);
    let zone = &server.zones[1];
    let other = name("other.example");
    lookup.response.answers.clear();
    zone.append_signed(
        &mut lookup.response.answers,
        zone.rrset(&other, ResourceType::A),
        &other,
        Some(&name("www.example")),
    );
    assert_eq!(
        lookup.finish(&mut server, &mut validator, NOW),
        Err("RRset signature did not validate")
    );
}

#[test]
fn child_key_must_match_ds() {
    // Re-key the child after the parent recorded its DS.
    let (mut server, mut validator) = setup(false, |server| {
        let mut rekeyed = Zone::create("example", false);
        rekeyed.add_a("www.example", Ipv4Addr::new(192, 0, 2, 1));
        server.zones[1] = rekeyed;
    });

    assert!(validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).is_err());
}

#[test]
fn unsigned_delegation_is_insecure() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "host.unsigned.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Insecure);
        assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 4)));
    }
}

#[test]
fn unsigned_delegation_without_proof_is_bogus() {
    let (mut server, mut validator) = setup(false, |server| server.zones[1].omit_denial_records = true);

    assert!(validated_lookup(&mut server, &mut validator, "host.unsigned.example", ResourceType::A).is_err());
}

#[test]
fn untrusted_root_key_is_bogus() {
    let (mut server, mut validator) = setup(false, |_| {});
    // Anchor some other key instead of the server's root key.
    validator.set_trust_anchors(vec![Zone::create("", false).ds()]);

    assert!(validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).is_err());
}

#[test]
fn slow_validation_does_not_hold_up_other_lookups() {
    let (mut server, mut validator) = setup(false, |server| {
        let mut test = Zone::create("test", false);
        test.add_a("www.test", Ipv4Addr::new(192, 0, 2, 5));
        server.zones[0].delegate(&test);
        server.zones.push(test);
    });

    // Runs a lookup until it waits for the DS query at `held`, without answering that one.
    let run_until_held = |server: &mut Server, validator: &mut Validator, lookup: &mut Lookup, held: &str| loop {
        match lookup.step(validator, NOW) {
            Step::NeedFetch(question) if question.rtype == ResourceType::DS && question.name == name(held) => {
                return question;
            }
            Step::NeedFetch(question) => lookup.fetch(server, &question),
            step => panic!("lookup finished before reaching the held query: {step:?}"),
        }
    };

    // Both validations end up waiting on a held DS query, the second one starting while the first is waiting.
    let mut example = Lookup::start(&mut server, "www.example", ResourceType::A);
    let example_held = run_until_held(&mut server, &mut validator, &mut example, "example");
    let mut test = Lookup::start(&mut server, "www.test", ResourceType::A);
    let test_held = run_until_held(&mut server, &mut validator, &mut test, "test");

    // Finishing the first validation must not have to wait for the second.
    example.fetch(&mut server, &example_held);
    let example_result = example.finish(&mut server, &mut validator, NOW).unwrap();
    assert_eq!(example_result.status, SecurityStatus::Secure);
    assert_eq!(test.step(&mut validator, NOW), Step::NeedFetch(test_held.clone()));

    test.fetch(&mut server, &test_held);
    let test_result = test.finish(&mut server, &mut validator, NOW).unwrap();
    assert_eq!(test_result.status, SecurityStatus::Secure);
    assert_eq!(first_address(&test_result), Some(Ipv4Addr::new(192, 0, 2, 5)));
}

#[test]
fn signatures_outside_their_validity_period_are_bogus() {
    for now in [NOW - 2 * 3600, NOW + 2 * 3600] {
        let (mut server, mut validator) = setup(false, |_| {});

        assert!(validated_lookup_at(&mut server, &mut validator, "www.example", ResourceType::A, now).is_err());
    }
}

#[test]
fn validity_period_uses_serial_number_arithmetic() {
    // Signatures valid from an hour before the 32-bit wrap-around to an hour after it.
    let signing_time = u32::MAX - 100;
    let (mut server, mut validator) = setup(false, |server| {
        for zone in &mut server.zones {
            zone.signing_time = signing_time;
        }
    });

    for now in [signing_time, signing_time.wrapping_add(1000)] {
        validator.clear_caches();
        let result = validated_lookup_at(&mut server, &mut validator, "www.example", ResourceType::A, now).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
    }
    validator.clear_caches();
    assert!(
        validated_lookup_at(
            &mut server,
            &mut validator,
            "www.example",
            ResourceType::A,
            signing_time.wrapping_add(7200)
        )
        .is_err()
    );
}

#[test]
fn ds_with_unsupported_digest_type_is_insecure() {
    // RFC 4035, 5.2: no supported authentication path leads to the child, as with a proven absence of DS.
    let (mut server, mut validator) = setup(false, |server| {
        let records = server.zones[0]
            .records
            .get_mut(&name("example").to_canonical_string())
            .unwrap();
        for record in records {
            if let RecordData::Ds(ds) = &mut record.data {
                ds.digest_type = DigestType(99);
                ds.digest = vec![0xab; 7];
            }
        }
    });

    let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    assert_eq!(result.status, SecurityStatus::Insecure);
    assert_eq!(result.reason, "No supported DS algorithm");
    assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 1)));
}

#[test]
fn cached_chain_expires() {
    let (mut server, mut validator) = setup(false, |_| {});

    validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    let queries_before = server.queries;
    validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    assert_eq!(server.queries - queries_before, 1);

    // The keys and the DS for "example" live for an hour, but the NSEC proving www.example is not a zone cut only
    // for 300 seconds.
    let queries_before = server.queries;
    validated_lookup_at(&mut server, &mut validator, "www.example", ResourceType::A, NOW + 301).unwrap();
    assert_eq!(server.queries - queries_before, 2);
}

#[test]
fn cached_chain_expires_counting_from_when_responses_came_in() {
    let (mut server, mut validator) = setup(false, |_| {});

    // Every DS and DNSKEY response came in 3000 seconds before validation ran.
    let received_at = NOW - 3000;
    let mut lookup = Lookup::start(&mut server, "www.example", ResourceType::A);
    while let Step::NeedFetch(question) = lookup.step(&mut validator, NOW) {
        let response = server.answer(&question);
        lookup.fetches.insert_received_at(&question, response, received_at);
    }

    // The DS and DNSKEY RRsets of "example" have a TTL of an hour.
    let example = name("example");
    assert_eq!(validator.zone_keys[&example].expires, u64::from(received_at) + 3600);
    assert_eq!(
        validator.delegations[&example].delegation.expires,
        u64::from(received_at) + 3600
    );
}

#[test]
fn cached_delegation_holds_only_under_its_parent_zone() {
    let (mut server, mut validator) = setup(false, |_| {});
    validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();

    // A decision that "www.example" is an unsigned delegation, made by some other zone.
    let cached = validator.delegations.get_mut(&name("www.example")).unwrap();
    cached.parent_zone = name("other");
    cached.delegation.kind = DelegationKind::Insecure;

    let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    assert_eq!(result.status, SecurityStatus::Secure);
    assert_eq!(validator.delegations[&name("www.example")].parent_zone, name("example"));
}

#[test]
fn cached_keys_are_reauthenticated_after_ds_rollover() {
    // The DS for "example" lives for 600 seconds, its DNSKEY RRset for an hour.
    let (mut server, mut validator) = setup(false, |server| {
        for record in server.zones[0]
            .records
            .get_mut(&name("example").to_canonical_string())
            .unwrap()
        {
            if record.rtype == ResourceType::DS {
                record.ttl = 600;
            }
        }
    });
    let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    assert_eq!(result.status, SecurityStatus::Secure);

    // Roll the key of "example" over to a new one, and the DS in the root along with it.
    let mut rekeyed = make_server(false).zones.remove(1);
    rekeyed.finalize();
    let new_ds = rekeyed.ds();
    for record in server.zones[0]
        .records
        .get_mut(&name("example").to_canonical_string())
        .unwrap()
    {
        if let RecordData::Ds(ds) = &mut record.data {
            *ds = new_ds.clone();
        }
    }
    server.zones[1] = rekeyed;

    let result = validated_lookup_at(&mut server, &mut validator, "www.example", ResourceType::A, NOW + 601).unwrap();
    assert_eq!(result.status, SecurityStatus::Secure);
    assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 1)));
}

#[test]
fn root_trust_anchors_are_the_iana_ones() {
    let anchors = Validator::root_trust_anchors();
    assert_eq!(anchors.len(), 2);
    assert_eq!(anchors[0].key_tag, 20326);
    assert_eq!(anchors[1].key_tag, 38696);
    assert_eq!(anchors[0].digest[..4], [0xe0, 0x6d, 0x44, 0xb8]);
    assert_eq!(anchors[1].digest[28..], [0xc0, 0xfb, 0x2b, 0x16]);
}

#[test]
fn failed_query_fails_validation() {
    let mut validator = Validator::new();
    let mut response = Message::default();
    response.header.options.set_response_code(ResponseCode::SERVER_FAILURE);
    let question = Question {
        name: name("www.example"),
        rtype: ResourceType::A,
        class: Class::IN,
    };
    assert_eq!(
        validator.validate(&response, &question, &Fetches::new(), NOW),
        Step::Failed("DNS query failed")
    );
}
#[test]
fn rsa_keys_below_1024_bits_are_not_used() {
    let rsa_key = |modulus: &str| {
        let mut public_key = vec![3, 0x01, 0x00, 0x01];
        public_key.extend(super::decode_hex(modulus));
        Dnskey {
            flags: 257,
            protocol: 3,
            algorithm: Algorithm::RSASHA256,
            public_key,
        }
    };

    // RSA/SHA-256 signatures of the same data by a 1024-bit and a 1020-bit key.
    let key = rsa_key(concat!(
        "F26D18767150BE3525C56A40C427D2BBA2D000C9A0DA1DAD6CFEAD6C4A4BAB96DC006781A0DB4C94A7ED7B46B4E6EEDC",
        "A7F7A84D96DDA6CA7C3F9A7788F64835310664658930AA980193BC352B56C4AD3A787279DDFA3DDACF95FA9E4ED99CFC",
        "8F0D1612FBD32111B0AF08A707ACA6C69DA232985E055A8C6F1ECF9E2E0BB7F7",
    ));
    let signature = super::decode_hex(concat!(
        "9fbdd45cf967014402559067f8d3759d006ee61887d35d82a8ef95ec2a6ff6db1bb50c16aafb83c1cc4e5fd1a988686a",
        "d5f7991b50d5b00887f97c3e2408d25fc2374139406bd4c2bd3117d09b4bbadc2fd8bcd50133ddfb47d2221f5fded178",
        "5e775e3ed92734d1506d0cf99dd81cf27f76b7e87ed6d63e4fd7a3fcae416c27",
    ));
    let short_key = rsa_key(concat!(
        "0E1C968C2452C3AE8BA8BBF0FCD69D6D0AE85A7CEFD1D93057132FE749F27114E81919F97CE97F685C1D7F7A36D3240F",
        "4CB833B3D0C1B98F48BC0A4DA0441E6653108F2CB6897152C5D250E376473297F2C7F7D6BC2E8EDC061C60EA8429F78E",
        "BA57378B09475FDBB0D18C11E7C1131627C0C82BF558AF74CA93A25B06E4885F",
    ));
    let short_signature = super::decode_hex(concat!(
        "044fe9134d788b92237fb5757fa30d7878ff56204db5024453603b1d0e4013abdda3005cd9d1d9ec44a0b325d096abc2",
        "06b681cb2b5020ad5375c17e87dcaae082a09f0855bc72d7559991ccf0cbf5f893217eae83899acd202f59166cedf71f",
        "c1ba4024b121d3bdf6f38a0314bb96dbae6410fed9b68f6fcd30c0883f036f66",
    ));

    assert!(verify_signature(&key, b"signed data", &signature));
    assert!(!verify_signature(&short_key, b"signed data", &short_signature));
}

#[test]
fn rsa_keys_with_public_exponents_over_33_bits_are_not_used() {
    // An RSA/SHA-256 signature by a 1024-bit key with the 34-bit public exponent 0x200000011.
    let mut public_key = vec![5, 0x02, 0x00, 0x00, 0x00, 0x11];
    public_key.extend(super::decode_hex(concat!(
        "B548B7328D8CB02AF4C70EFD8CB2A7CFDCA60EAAA5DEEB5072DD73D1FAE56890B4E532C50CA4CFAFCC59B10D25250C04",
        "BDF3EE018D8797BD2307FFC7E66819340B145238A06659280AB3DC815304DE4EDA75FCD6B4DF9E164F4B00D57C2BA7D2",
        "865D1E1EBE1C5D6EA7FCB21ECB58419EDDD4BD09CAEAF0EFD3C96F393FB59E3B",
    )));
    let key = Dnskey {
        flags: 257,
        protocol: 3,
        algorithm: Algorithm::RSASHA256,
        public_key,
    };
    let signature = super::decode_hex(concat!(
        "8eee919590793c513f396164c8fb880013be374e366a3dd10da353d95fbfde5a76d4210831583b5f85613f8ba0915243",
        "ac5b8c88ae688e95966c46cf383dbf97ae4e079d4722d3d24323b9db8b7d09f714233d38f48b6e6d48b7f3224e036736",
        "101001f511d08effc4cce25db428834b55699597244d62fbd4672226f9948f69",
    ));
    assert!(!verify_signature(&key, b"signed data", &signature));
}

fn unsigned_nsec3(
    zone: &DomainName,
    owner_hash: &[u8],
    next_hash: &[u8],
    flags: u8,
    types: &[ResourceType],
) -> VerifiedRRSet {
    let owner = zone.with_label_prepended(base32hex_encode(owner_hash));
    let nsec3 = Nsec3 {
        hash_algorithm: Nsec3HashAlgorithm::SHA1,
        flags,
        iterations: 0,
        salt: Vec::new(),
        next_hashed_owner_name: next_hash.to_vec(),
        types: types.to_vec(),
    };
    unverified_rrset(zone, &owner, ResourceType::NSEC3, RecordData::Nsec3(nsec3))
}

fn unsigned_nsec(zone: &DomainName, owner: &str, next: &str, types: &[ResourceType]) -> VerifiedRRSet {
    let nsec = Nsec {
        next_domain_name: name(next),
        types: types.to_vec(),
    };
    unverified_rrset(zone, &name(owner), ResourceType::NSEC, RecordData::Nsec(nsec))
}

// A one-record RRset of `zone`, passed off as verified.
fn unverified_rrset(zone: &DomainName, owner: &DomainName, rtype: ResourceType, data: RecordData) -> VerifiedRRSet {
    VerifiedRRSet {
        rrset: SignedRRSet {
            owner: owner.clone(),
            rtype,
            class: Class::IN,
            records: vec![record(owner, rtype, data, 300)],
            rrsigs: Vec::new(),
        },
        zone: zone.clone(),
        wildcard_label_count: None,
        ttl: 300,
    }
}

#[test]
fn opt_out_proof_does_not_depend_on_record_order() {
    let zone = name("example");
    let delegation = name("insecure.example");
    let apex = unsigned_nsec3(
        &zone,
        &nsec3_hash(&zone),
        &[0; 20],
        Nsec3::FLAG_OPT_OUT,
        &[ResourceType::SOA, ResourceType::NS, ResourceType::NSEC3PARAM],
    );
    // Two records whose spans both cover the hash of the delegation name.
    let opt_out = unsigned_nsec3(&zone, &[0x00; 20], &[0xff; 20], Nsec3::FLAG_OPT_OUT, &[ResourceType::A]);
    let mut first = [0x00; 20];
    first[19] = 1;
    let mut last = [0xff; 20];
    last[19] = 0xfe;
    let no_opt_out = unsigned_nsec3(&zone, &first, &last, 0, &[ResourceType::A]);
    let both_opt_out = unsigned_nsec3(&zone, &first, &last, Nsec3::FLAG_OPT_OUT, &[ResourceType::A]);

    let proves_insecure = |rrsets: &[VerifiedRRSet]| DenialProofs { rrsets }.proves_insecure_delegation(&delegation);
    assert!(!proves_insecure(&[apex.clone(), opt_out.clone(), no_opt_out.clone()]));
    assert!(!proves_insecure(&[apex.clone(), no_opt_out, opt_out.clone()]));
    assert!(proves_insecure(&[apex.clone(), opt_out.clone(), both_opt_out.clone()]));
    assert!(proves_insecure(&[apex, both_opt_out, opt_out]));
}

#[test]
fn parent_side_denial_says_nothing_below_the_delegation() {
    let zone = name("example");
    let sub = name("sub.example");

    // The NSEC at the delegation point "sub.example" spans every name below it.
    let rrsets = [unsigned_nsec(
        &zone,
        "sub.example",
        "zzz.example",
        &[ResourceType::NS, ResourceType::RRSIG, ResourceType::NSEC],
    )];
    let proofs = DenialProofs { rrsets: &rrsets };
    assert_eq!(proofs.proves_no_data(&sub, ResourceType::DS), Some(Denial::Proven));
    assert!(proofs.proves_no_data(&sub, ResourceType::A).is_none());
    assert!(proofs.proves_name_error(&name("www.sub.example")).is_none());
    assert!(
        proofs
            .proves_no_data(&name("www.sub.example"), ResourceType::A)
            .is_none()
    );

    let rrsets = [unsigned_nsec3(
        &zone,
        &nsec3_hash(&sub),
        &[0xff; 20],
        0,
        &[ResourceType::NS],
    )];
    let proofs = DenialProofs { rrsets: &rrsets };
    assert_eq!(proofs.proves_no_data(&sub, ResourceType::DS), Some(Denial::Proven));
    assert!(proofs.proves_no_data(&sub, ResourceType::A).is_none());

    // Nor does the NSEC at a DNAME.
    let rrsets = [unsigned_nsec(
        &zone,
        "old.example",
        "zzz.example",
        &[ResourceType::DNAME, ResourceType::RRSIG, ResourceType::NSEC],
    )];
    let proofs = DenialProofs { rrsets: &rrsets };
    assert_eq!(
        proofs.proves_no_data(&name("old.example"), ResourceType::A),
        Some(Denial::Proven)
    );
    assert!(proofs.proves_name_error(&name("www.old.example")).is_none());

    // Within the signer's zone, including at its apex, the bit maps still count.
    let rrsets = [
        unsigned_nsec(
            &zone,
            "example",
            "www.example",
            &[
                ResourceType::NS,
                ResourceType::SOA,
                ResourceType::RRSIG,
                ResourceType::NSEC,
            ],
        ),
        unsigned_nsec(
            &zone,
            "www.example",
            "zzz.example",
            &[ResourceType::A, ResourceType::RRSIG, ResourceType::NSEC],
        ),
    ];
    let proofs = DenialProofs { rrsets: &rrsets };
    assert_eq!(proofs.proves_no_data(&zone, ResourceType::A), Some(Denial::Proven));
    assert_eq!(
        proofs.proves_no_data(&name("www.example"), ResourceType::AAAA),
        Some(Denial::Proven)
    );
}

// An NSEC3 at the apex of "example", whose span covers next to nothing, and one that covers the hash of every other
// name, with or without Opt-Out.
fn apex_and_covering_nsec3(flags: u8) -> [VerifiedRRSet; 2] {
    let zone = name("example");
    let apex_hash = nsec3_hash(&zone);
    let mut next_hash = apex_hash.clone();
    next_hash[19] = next_hash[19].wrapping_add(1);
    [
        unsigned_nsec3(&zone, &apex_hash, &next_hash, 0, &[ResourceType::NS, ResourceType::SOA]),
        unsigned_nsec3(&zone, &[0x00; 20], &[0xff; 20], flags, &[ResourceType::A]),
    ]
}

#[test]
fn name_error_proven_by_an_opt_out_span_is_insecure() {
    let missing = name("missing.example");
    let rrsets = apex_and_covering_nsec3(Nsec3::FLAG_OPT_OUT);
    assert_eq!(
        DenialProofs { rrsets: &rrsets }.proves_name_error(&missing),
        Some(Denial::OptOut)
    );
    let rrsets = apex_and_covering_nsec3(0);
    assert_eq!(
        DenialProofs { rrsets: &rrsets }.proves_name_error(&missing),
        Some(Denial::Proven)
    );
}

#[test]
fn no_data_proven_by_an_opt_out_span_is_insecure() {
    // An empty non-terminal above an unsigned delegation can be left out of an Opt-Out zone's NSEC3 chain.
    let empty_non_terminal = name("ent.example");
    let rrsets = apex_and_covering_nsec3(Nsec3::FLAG_OPT_OUT);
    assert_eq!(
        DenialProofs { rrsets: &rrsets }.proves_no_data(&empty_non_terminal, ResourceType::A),
        Some(Denial::OptOut)
    );
    // Without Opt-Out, the name does not exist, and there is no wildcard for it to have no data at.
    let rrsets = apex_and_covering_nsec3(0);
    assert_eq!(
        DenialProofs { rrsets: &rrsets }.proves_no_data(&empty_non_terminal, ResourceType::A),
        None
    );
}

#[test]
fn nsec3_endpoints_must_be_sha1_hashes() {
    let zone = name("example");
    let proves_wildcard_absent = |owner_hash: &[u8], next_hash: &[u8]| {
        let rrsets = [unsigned_nsec3(&zone, owner_hash, next_hash, 0, &[ResourceType::A])];
        DenialProofs { rrsets: &rrsets }.proves_wildcard_absent(&zone)
    };
    assert!(proves_wildcard_absent(&[0x00; 20], &[0xff; 20]));
    assert!(!proves_wildcard_absent(&[0x00; 19], &[0xff; 19]));
    assert!(!proves_wildcard_absent(&[0x00; 21], &[0xff; 21]));
}

#[test]
fn insecure_delegations_for_too_many_nsec3_iterations_expire_with_their_proof() {
    // The NSEC3 records of "example" take too many iterations, and their signatures expire in two seconds.
    let (mut server, mut validator) = setup(true, |server| {
        server.zones[1].nsec3_iterations = 101;
        server.expire_example_signatures_in(2);
    });
    let result = validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).unwrap();
    assert_eq!(result.status, SecurityStatus::Insecure);
    let cached = &validator.delegations[&name("www.example")].delegation;
    assert_eq!(cached.kind, DelegationKind::Insecure);
    assert_eq!(cached.expires, u64::from(NOW) + 2);
}

fn with_dname(server: &mut Server) {
    server.zones[1].add_dname("mirror.example", "example", 120);
}

#[test]
fn answers_through_a_signed_dname_validate() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, with_dname);
        let result = validated_lookup(&mut server, &mut validator, "www.mirror.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 1)));
        // What the DNAME leads to lives no longer than the DNAME.
        assert!(result.records.iter().all(|record| record.ttl <= 120));
        assert!(result.records.iter().any(|record| record.rtype == ResourceType::DNAME));

        let result = validated_lookup(&mut server, &mut validator, "www.mirror.example", ResourceType::CNAME).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert!(result.records.iter().any(|record| record.rtype == ResourceType::CNAME));
    }
}

#[test]
fn cnames_that_do_not_follow_from_their_dname_are_bogus() {
    let (mut server, mut validator) = setup(false, with_dname);
    let mut lookup = Lookup::start(&mut server, "www.mirror.example", ResourceType::A);
    for answer in &mut lookup.response.answers {
        if answer.rtype == ResourceType::CNAME {
            answer.data = RecordData::Cname(name("other.example"));
        }
    }
    assert_eq!(
        lookup.finish(&mut server, &mut validator, NOW).err(),
        Some("CNAME does not follow from the DNAME above it")
    );
}

#[test]
fn unsigned_dnames_do_not_vouch_for_their_cnames() {
    let (mut server, mut validator) = setup(false, with_dname);
    let mut lookup = Lookup::start(&mut server, "www.mirror.example", ResourceType::A);
    lookup.response.answers.retain(
        |answer| !matches!(&answer.data, RecordData::Rrsig(rrsig) if rrsig.type_covered == ResourceType::DNAME),
    );
    assert_eq!(
        lookup.finish(&mut server, &mut validator, NOW).err(),
        Some("RRset signature did not validate")
    );
}
