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
use super::denial::DenialProofs;
use super::rrset::{SignedRRSet, VerifiedRRSet};
use super::{Fetches, SecurityStatus, Step, ValidationResult, Validator};
use crate::message::{
    Algorithm, Class, DigestType, Dnskey, DomainName, Ds, Message, Nsec, Nsec3, Nsec3HashAlgorithm, Nsec3Param,
    Question, RecordData, ResourceRecord, ResourceType, ResponseCode, Sig, Soa, Txt,
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
    key_pair: Option<KeyPair>,
    dnskey: Dnskey,
    key_tag: u16,
    // Records keyed by canonical owner name, without further grouping.
    records: HashMap<Vec<u8>, Vec<ResourceRecord>>,
    names: Vec<DomainName>,
    delegations: Vec<DomainName>,
    nsec_records: Vec<ResourceRecord>,
    signing_time: u32,

    corrupt_signatures: bool,
    omit_denial_records: bool,
    omit_wildcard_proof: bool,
    spoof_answer_owner: Option<DomainName>,
}

impl Zone {
    fn empty(apex: &str) -> Self {
        Self {
            apex: name(apex),
            signed: true,
            use_nsec3: false,
            key_pair: None,
            dnskey: Dnskey::default(),
            key_tag: 0,
            records: HashMap::new(),
            names: Vec::new(),
            delegations: Vec::new(),
            nsec_records: Vec::new(),
            signing_time: NOW,
            corrupt_signatures: false,
            omit_denial_records: false,
            omit_wildcard_proof: false,
            spoof_answer_owner: None,
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
        for record in &self.records[&name.to_canonical_string()] {
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
            sorted_names.sort_by_key(nsec3_hash);
            let apex = self.apex.clone();
            self.add(
                &apex,
                ResourceType::NSEC3PARAM,
                RecordData::Nsec3Param(Nsec3Param {
                    hash_algorithm: Nsec3HashAlgorithm::SHA1,
                    flags: 0,
                    iterations: 0,
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
                    iterations: 0,
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
        if self.corrupt_signatures {
            rrsig.signature[0] ^= 0x01;
        }
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
                for candidate in &self.names {
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
        while !self.has_name(&candidate) && candidate.labels.len() > self.apex.labels.len() {
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

                if self.has_name(&name) {
                    let exact = self.rrset(&name, question.rtype);
                    if !exact.is_empty() {
                        self.append_signed(&mut answers, exact, &name, self.spoof_answer_owner.as_ref());
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
                    // No Data.
                    self.append_signed(
                        &mut authority,
                        self.rrset(&self.apex, ResourceType::SOA),
                        &self.apex,
                        None,
                    );
                    self.append_denial(&mut authority, self.nsec_matching(&name));
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
fn wildcard_answer_validates_with_proof() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "anything.wild.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert_eq!(first_address(&result), Some(Ipv4Addr::new(192, 0, 2, 3)));
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
fn name_error_is_proven() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |_| {});

        let result = validated_lookup(&mut server, &mut validator, "missing.example", ResourceType::A).unwrap();
        assert_eq!(result.status, SecurityStatus::Secure);
        assert!(result.records.is_empty());
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

#[test]
fn name_error_without_proof_is_bogus() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |server| server.zones[1].omit_denial_records = true);

        assert!(validated_lookup(&mut server, &mut validator, "missing.example", ResourceType::A).is_err());
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

#[test]
fn no_data_without_proof_is_bogus() {
    for use_nsec3 in [false, true] {
        let (mut server, mut validator) = setup(use_nsec3, |server| server.zones[1].omit_denial_records = true);

        assert!(validated_lookup(&mut server, &mut validator, "www.example", ResourceType::AAAA).is_err());
    }
}

#[test]
fn bad_signature_is_bogus() {
    let (mut server, mut validator) = setup(false, |server| server.zones[1].corrupt_signatures = true);

    assert!(validated_lookup(&mut server, &mut validator, "www.example", ResourceType::A).is_err());
}

#[test]
fn answer_for_another_name_is_bogus() {
    // The signed RRset for other.example is presented under the queried name.
    let (mut server, mut validator) = setup(false, |server| {
        server.zones[1].spoof_answer_owner = Some(name("www.example"));
    });

    assert!(validated_lookup(&mut server, &mut validator, "other.example", ResourceType::A).is_err());
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
    VerifiedRRSet {
        rrset: SignedRRSet {
            owner: owner.clone(),
            rtype: ResourceType::NSEC3,
            class: Class::IN,
            records: vec![record(&owner, ResourceType::NSEC3, RecordData::Nsec3(nsec3), 300)],
            rrsigs: Vec::new(),
            rrsig_ttl: 300,
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
