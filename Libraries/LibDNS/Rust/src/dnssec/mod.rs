/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DNSSEC validation (RFC 4033, RFC 4034, RFC 4035, RFC 5155). The validator never does I/O: when it needs a DS or
//! DNSKEY response it does not have yet, it asks for it, and the caller runs it again once that response is in
//! `Fetches`.

mod crypto;
mod denial;
mod rrset;

#[cfg(test)]
pub(crate) mod tests;

use std::collections::HashMap;
use std::fmt;

use crate::message::{
    Algorithm, Class, DigestType, Dnskey, DomainName, Ds, Message, Question, RecordData, ResourceRecord, ResourceType,
    ResponseCode,
};

use crypto::{ds_matches_key, is_supported_algorithm, is_supported_digest_type, is_usable_zone_key};
use denial::{Denial, DenialProofs, has_excessive_nsec3_iterations};
use rrset::{SignedRRSet, group_rrsets, verify_denial_records, verify_rrset};

/// RFC 4035, 4.3. Determining Security Status of Data.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum SecurityStatus {
    Secure,
    Insecure,
    #[default]
    Bogus,
}

impl SecurityStatus {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Secure => "Secure",
            Self::Insecure => "Insecure",
            Self::Bogus => "Bogus",
        }
    }
}

impl fmt::Display for SecurityStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ValidationResult {
    pub status: SecurityStatus,
    /// The records answering the question, following CNAMEs. Authenticated when the status is Secure.
    pub records: Vec<ResourceRecord>,
    pub reason: &'static str,
    /// For a Name Error or No Data answer proven by NSEC or NSEC3 records, how many more seconds the proof holds: the
    /// lowest TTL among the authenticated NSEC and NSEC3 RRsets of the response, each already capped by the expiration
    /// of its signature. A negative answer must not be cached for longer. `None` when no such proof was used.
    pub denial_ttl: Option<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Step {
    Done(ValidationResult),
    /// Validation needs the response to this question in `Fetches` before it can go on.
    NeedFetch(Question),
    /// The response could not be validated at all, e.g. a needed query failed or a chain of trust is broken.
    Failed(&'static str),
}

/// The DS and DNSKEY responses one validation has fetched so far.
#[derive(Clone, Debug, Default)]
pub struct Fetches {
    // Keyed by canonicalized names, like the validator's caches.
    responses: HashMap<(DomainName, ResourceType), FetchedResponse>,
}

#[derive(Clone, Debug)]
struct FetchedResponse {
    message: Message,
    /// Seconds since the epoch, or `None` to count from the time validation runs.
    received_at: Option<u32>,
}

impl Fetches {
    pub fn new() -> Self {
        Self::default()
    }

    /// Records the response to a question asked for by `Step::NeedFetch`, as if it had come in when validation runs.
    pub fn insert(&mut self, question: &Question, response: Message) {
        self.insert_response(question, response, None);
    }

    /// Records the response to a question asked for by `Step::NeedFetch`, which came in at `received_at` (seconds
    /// since the epoch). The lifetimes of what is cached from it count from then.
    pub fn insert_received_at(&mut self, question: &Question, response: Message, received_at: u32) {
        self.insert_response(question, response, Some(received_at));
    }

    fn insert_response(&mut self, question: &Question, message: Message, received_at: Option<u32>) {
        self.responses.insert(
            (question.name.canonicalized(), question.rtype),
            FetchedResponse { message, received_at },
        );
    }

    pub fn get(&self, name: &DomainName, rtype: ResourceType) -> Option<&Message> {
        self.responses
            .get(&(name.canonicalized(), rtype))
            .map(|fetched| &fetched.message)
    }

    pub fn len(&self) -> usize {
        self.responses.len()
    }

    pub fn is_empty(&self) -> bool {
        self.responses.is_empty()
    }
}

const MAX_CACHED_ENTRIES: usize = 1024;
const EXCESSIVE_NSEC3_ITERATIONS: &str = "NSEC3 iteration count is too high";
const OPT_OUT: &str = "Denial is covered by an NSEC3 Opt-Out span";
const MAX_CNAME_CHAIN_LENGTH: usize = 16;

#[derive(Clone, Debug, PartialEq, Eq)]
enum Failure {
    Fetch(Question),
    Error(&'static str),
}

/// The closest enclosing zone of a name for which a chain of trust has been established (or found broken).
struct Chain {
    status: SecurityStatus,
    zone: DomainName,
    keys: Vec<Dnskey>,
    reason: &'static str,
}

type CanonicalDsSet = Vec<(u16, Algorithm, DigestType, Vec<u8>)>;

fn canonical_ds_set(ds_set: &[Ds]) -> CanonicalDsSet {
    let mut canonical: CanonicalDsSet = ds_set
        .iter()
        .map(|ds| (ds.key_tag, ds.algorithm, ds.digest_type, ds.digest.clone()))
        .collect();
    canonical.sort();
    canonical.dedup();
    canonical
}

#[derive(Clone, Debug)]
struct ZoneKeys {
    keys: Vec<Dnskey>,
    /// The DS set the keys were authenticated with. Keys are only good for that set: once the parent has published
    /// a different one, e.g. in a key rollover, they have to be authenticated again.
    ds_set: CanonicalDsSet,
    /// Seconds since the epoch.
    expires: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DelegationKind {
    Secure,
    Insecure,
    NotACut,
    NonExistent,
}

#[derive(Clone, Debug)]
struct Delegation {
    kind: DelegationKind,
    ds: Vec<Ds>,
    /// Seconds since the epoch.
    expires: u64,
}

#[derive(Clone, Debug)]
struct CachedDelegation {
    /// The zone whose keys authenticated the decision. It only holds while that is still the parent zone.
    parent_zone: DomainName,
    delegation: Delegation,
}

enum Accepted {
    Records(Vec<ResourceRecord>),
    Insecure(&'static str),
    Bogus(&'static str),
}

fn decode_hex(hex: &str) -> Vec<u8> {
    let digit = |ch: u8| match ch {
        b'0'..=b'9' => ch - b'0',
        b'a'..=b'f' => ch - b'a' + 10,
        b'A'..=b'F' => ch - b'A' + 10,
        _ => unreachable!("invalid hex digit in a built-in trust anchor"),
    };
    hex.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|[high, low]| digit(*high) << 4 | digit(*low))
        .collect()
}

fn is_positive_or_name_error(message: &Message) -> bool {
    let rcode = message.header.options.response_code();
    rcode == ResponseCode::NO_ERROR || rcode == ResponseCode::NAME_ERROR
}

// The fetched response, and when it came in.
fn fetched<'a>(
    fetches: &'a Fetches,
    name: &DomainName,
    rtype: ResourceType,
    now: u32,
) -> Result<(&'a Message, u32), Failure> {
    fetches
        .responses
        .get(&(name.canonicalized(), rtype))
        .map(|fetched| {
            (
                &fetched.message,
                fetched.received_at.map_or(now, |received_at| received_at.min(now)),
            )
        })
        .ok_or_else(|| {
            Failure::Fetch(Question {
                name: name.clone(),
                rtype,
                class: Class::IN,
            })
        })
}

// The DNAME among the RRsets whose owner is an ancestor of `name`, which a CNAME at `name` may have been synthesized
// from.
fn dname_above<'a>(rrsets: &'a [SignedRRSet], name: &DomainName, class: Class) -> Option<&'a SignedRRSet> {
    rrsets.iter().find(|rrset| {
        rrset.rtype == ResourceType::DNAME
            && rrset.class == class
            && rrset.owner.labels.len() < name.labels.len()
            && rrset.owner.is_ancestor_or_equal_of(name)
    })
}

// The target of `cname`, if it is what substituting the owner of `dname` with its target in `name` makes.
fn synthesized_target(cname: &SignedRRSet, dname: &SignedRRSet, name: &DomainName) -> Option<DomainName> {
    let [cname_record] = cname.records.as_slice() else {
        return None;
    };
    let [dname_record] = dname.records.as_slice() else {
        return None;
    };
    let (RecordData::Cname(target), RecordData::Dname(dname_target)) = (&cname_record.data, &dname_record.data) else {
        return None;
    };
    let kept = name.labels.len() - dname.owner.labels.len();
    let mut expected = DomainName {
        labels: name.labels[..kept].to_vec(),
    };
    expected.labels.extend(dname_target.labels.iter().cloned());
    expected.eq_ignore_case(target).then(|| target.clone())
}

fn expiry(received_at: u32, ttl: u32) -> u64 {
    u64::from(received_at) + u64::from(ttl)
}

#[derive(Clone, Debug)]
pub struct Validator {
    trust_anchors: Vec<Ds>,
    // Keyed by canonicalized names, which keep the labels apart: a label can hold a dot.
    zone_keys: HashMap<DomainName, ZoneKeys>,
    delegations: HashMap<DomainName, CachedDelegation>,
}

impl Default for Validator {
    fn default() -> Self {
        Self::new()
    }
}

impl Validator {
    pub fn new() -> Self {
        Self {
            trust_anchors: Self::root_trust_anchors(),
            zone_keys: HashMap::new(),
            delegations: HashMap::new(),
        }
    }

    // https://data.iana.org/root-anchors/root-anchors.xml
    pub fn root_trust_anchors() -> Vec<Ds> {
        vec![
            Ds {
                key_tag: 20326,
                algorithm: Algorithm::RSASHA256,
                digest_type: DigestType::SHA256,
                digest: decode_hex("E06D44B80B8F1D39A95C0B0D7C65D08458E880409BBC683457104237C7F8EC8D"),
            },
            Ds {
                key_tag: 38696,
                algorithm: Algorithm::RSASHA256,
                digest_type: DigestType::SHA256,
                digest: decode_hex("683D2D0ACB8C9B712A1948B27F741219298D0A450D612C483AF444A4C0FB2B16"),
            },
        ]
    }

    pub fn set_trust_anchors(&mut self, anchors: Vec<Ds>) {
        self.trust_anchors = anchors;
        self.clear_caches();
    }

    pub fn clear_caches(&mut self) {
        self.zone_keys.clear();
        self.delegations.clear();
    }

    /// Authenticates the answer to `question` in `response`. `now` is the current time in seconds since the epoch.
    /// Run it again with the same arguments, and the requested response added to `fetches`, until it is done.
    pub fn validate(&mut self, response: &Message, question: &Question, fetches: &Fetches, now: u32) -> Step {
        match self.try_validate(response, question, fetches, now) {
            Ok(result) => Step::Done(result),
            Err(Failure::Fetch(question)) => Step::NeedFetch(question),
            Err(Failure::Error(reason)) => Step::Failed(reason),
        }
    }

    // Empty when none of the DS records can be used to authenticate the zone, which makes the zone insecure.
    fn zone_keys(
        &mut self,
        zone: &DomainName,
        ds_set: &[Ds],
        fetches: &Fetches,
        now: u32,
    ) -> Result<Option<Vec<Dnskey>>, Failure> {
        let cache_key = zone.canonicalized();
        let canonical_ds = canonical_ds_set(ds_set);
        if let Some(cached) = self.zone_keys.get(&cache_key) {
            if cached.expires > u64::from(now) && cached.ds_set == canonical_ds {
                return Ok(Some(cached.keys.clone()));
            }
            self.zone_keys.remove(&cache_key);
        }

        // RFC 4035, 5.2. Authenticating Referrals.
        // If the validator does not support any of the algorithms listed in an authenticated DS RRset, then the resolver
        // has no supported authentication path leading from the parent to the child.  The resolver should treat this
        // case as it would the case of an authenticated NSEC RRset proving that no DS RRset exists, as described above.
        let usable_ds: Vec<&Ds> = ds_set
            .iter()
            .filter(|ds| is_supported_algorithm(ds.algorithm) && is_supported_digest_type(ds.digest_type))
            .collect();
        if usable_ds.is_empty() {
            return Ok(None);
        }

        let (response, received_at) = fetched(fetches, zone, ResourceType::DNSKEY, now)?;
        if !is_positive_or_name_error(response) {
            return Err(Failure::Error("DNSKEY query failed"));
        }

        let rrsets = group_rrsets(&response.answers);
        let dnskey_rrset = rrsets
            .iter()
            .find(|rrset| rrset.rtype == ResourceType::DNSKEY && rrset.owner.eq_ignore_case(zone))
            .ok_or(Failure::Error("Zone has no DNSKEY RRset"))?;

        // Given a DS RR for a delegation, the child zone's apex DNSKEY RRset can be authenticated if all of the following
        // hold:
        // o  The Algorithm and Key Tag in the DS RR match the Algorithm field and the key tag of a DNSKEY RR in the child
        //    zone's apex DNSKEY RRset, and, when the DNSKEY RR's owner name and RDATA are hashed using the digest
        //    algorithm specified in the DS RR's Digest Type field, the resulting digest value matches the Digest field
        //    of the DS RR.
        // o  The matching DNSKEY RR in the child zone has the Zone Flag bit set, the corresponding private key has
        //    signed the child zone's apex DNSKEY RRset, and the resulting RRSIG RR authenticates the child zone's apex
        //    DNSKEY RRset.
        for record in &dnskey_rrset.records {
            let RecordData::Dnskey(key) = &record.data else {
                continue;
            };
            if !is_usable_zone_key(key) {
                continue;
            }
            if !usable_ds.iter().any(|ds| ds_matches_key(zone, ds, key)) {
                continue;
            }

            let Some(verified) = verify_rrset(dnskey_rrset, zone, std::slice::from_ref(key), now)? else {
                continue;
            };

            let keys: Vec<Dnskey> = verified
                .rrset
                .records
                .iter()
                .filter_map(|record| match &record.data {
                    RecordData::Dnskey(zone_key) if is_usable_zone_key(zone_key) => Some(zone_key.clone()),
                    _ => None,
                })
                .collect();
            if self.zone_keys.len() >= MAX_CACHED_ENTRIES {
                self.zone_keys.clear();
            }
            self.zone_keys.insert(
                cache_key,
                ZoneKeys {
                    keys: keys.clone(),
                    ds_set: canonical_ds,
                    expires: expiry(received_at, verified.ttl),
                },
            );
            return Ok(Some(keys));
        }

        Err(Failure::Error("No DNSKEY matches an authenticated DS record"))
    }

    fn delegation(
        &mut self,
        name: &DomainName,
        parent: &Chain,
        fetches: &Fetches,
        now: u32,
    ) -> Result<Delegation, Failure> {
        let cache_key = name.canonicalized();
        if let Some(cached) = self.delegations.get(&cache_key) {
            if cached.delegation.expires > u64::from(now) && cached.parent_zone.eq_ignore_case(&parent.zone) {
                return Ok(cached.delegation.clone());
            }
            self.delegations.remove(&cache_key);
        }

        let mut remember = |delegation: Delegation| {
            if self.delegations.len() >= MAX_CACHED_ENTRIES {
                self.delegations.clear();
            }
            self.delegations.insert(
                cache_key.clone(),
                CachedDelegation {
                    parent_zone: parent.zone.clone(),
                    delegation: delegation.clone(),
                },
            );
            Ok(delegation)
        };

        let (response, received_at) = fetched(fetches, name, ResourceType::DS, now)?;
        if !is_positive_or_name_error(response) {
            return Err(Failure::Error("DS query failed"));
        }

        let answers = group_rrsets(&response.answers);
        // RFC 6672, 2.4. Names next to and below a DNAME Record
        // Resource records MUST NOT exist at any subdomain of the owner of a DNAME RR.
        // So no zone starts there either.
        if let Some(dname) = dname_above(&answers, name, Class::IN) {
            let verified = verify_rrset(dname, &parent.zone, &parent.keys, now)?
                .ok_or(Failure::Error("DNAME RRset did not validate"))?;
            return remember(Delegation {
                kind: DelegationKind::NotACut,
                ds: Vec::new(),
                expires: expiry(received_at, verified.ttl),
            });
        }
        for rrset in &answers {
            if !rrset.owner.eq_ignore_case(name) {
                continue;
            }
            // RFC 2181, 10.1: a name that is an alias cannot have any other data, so it is not a zone cut either.
            if rrset.rtype == ResourceType::CNAME {
                let verified = verify_rrset(rrset, &parent.zone, &parent.keys, now)?
                    .ok_or(Failure::Error("CNAME RRset did not validate"))?;
                return remember(Delegation {
                    kind: DelegationKind::NotACut,
                    ds: Vec::new(),
                    expires: expiry(received_at, verified.ttl),
                });
            }
            if rrset.rtype != ResourceType::DS {
                continue;
            }
            // RFC 4035, 5.2: The DS RR has been authenticated using some DNSKEY RR in the parent's apex DNSKEY RRset.
            let verified = verify_rrset(rrset, &parent.zone, &parent.keys, now)?
                .ok_or(Failure::Error("DS RRset did not validate"))?;
            let ds = verified
                .rrset
                .records
                .iter()
                .filter_map(|record| match &record.data {
                    RecordData::Ds(ds) => Some(ds.clone()),
                    _ => None,
                })
                .collect();
            return remember(Delegation {
                kind: DelegationKind::Secure,
                ds,
                expires: expiry(received_at, verified.ttl),
            });
        }

        // No DS: the parent has to prove that, with NSEC or NSEC3 records signed by its own keys.
        let authority = group_rrsets(&response.authorities);
        let verified = verify_denial_records(&authority, &parent.zone, &parent.keys, now)?;
        let insecure_or_not_a_cut = |kind, expires| Delegation {
            kind,
            ds: Vec::new(),
            expires,
        };
        let ttl = verified.iter().map(|rrset| rrset.ttl).min().unwrap_or(0);
        let expires = expiry(received_at, ttl);
        // The policy holds for a minute at most, and no longer than the records it is about.
        if has_excessive_nsec3_iterations(&verified) {
            return remember(insecure_or_not_a_cut(
                DelegationKind::Insecure,
                expiry(received_at, ttl.min(60)),
            ));
        }

        let proofs = DenialProofs { rrsets: &verified };
        if proofs.proves_insecure_delegation(name) {
            return remember(insecure_or_not_a_cut(DelegationKind::Insecure, expires));
        }
        if proofs.proves_name_exists_without_delegation(name) {
            return remember(insecure_or_not_a_cut(DelegationKind::NotACut, expires));
        }
        // Had an Opt-Out span covered the name, it would have made an insecure delegation above.
        if response.header.options.response_code() == ResponseCode::NAME_ERROR
            && proofs.proves_name_error(name).is_some()
        {
            return remember(insecure_or_not_a_cut(DelegationKind::NonExistent, expires));
        }
        // A No Data answer for DS without a matching NSEC means the name is an empty non-terminal or is covered by
        // a wildcard; either way it is not a zone cut.
        if proofs.proves_no_data(name, ResourceType::DS).is_some() {
            return remember(insecure_or_not_a_cut(DelegationKind::NotACut, expires));
        }

        Err(Failure::Error("Absence of DS record is not proven"))
    }

    fn chain_for(&mut self, name: &DomainName, fetches: &Fetches, now: u32) -> Result<Chain, Failure> {
        let root = DomainName::root();
        let anchors = self.trust_anchors.clone();
        let Some(root_keys) = self.zone_keys(&root, &anchors, fetches, now)? else {
            return Ok(Chain {
                status: SecurityStatus::Insecure,
                zone: root,
                keys: Vec::new(),
                reason: "No usable trust anchor",
            });
        };

        let mut chain = Chain {
            status: SecurityStatus::Secure,
            zone: root,
            keys: root_keys,
            reason: "",
        };
        for labels in 1..=name.labels.len() {
            let candidate = name.suffix(labels);
            let delegation = self.delegation(&candidate, &chain, fetches, now)?;
            match delegation.kind {
                DelegationKind::Secure => {
                    let Some(keys) = self.zone_keys(&candidate, &delegation.ds, fetches, now)? else {
                        return Ok(Chain {
                            status: SecurityStatus::Insecure,
                            zone: candidate,
                            keys: Vec::new(),
                            reason: "No supported DS algorithm",
                        });
                    };
                    chain = Chain {
                        status: SecurityStatus::Secure,
                        zone: candidate,
                        keys,
                        reason: "",
                    };
                }
                DelegationKind::Insecure => {
                    return Ok(Chain {
                        status: SecurityStatus::Insecure,
                        zone: candidate,
                        keys: Vec::new(),
                        reason: "Unsigned delegation",
                    });
                }
                DelegationKind::NotACut => {}
                DelegationKind::NonExistent => return Ok(chain),
            }
        }
        Ok(chain)
    }

    fn accept(
        &mut self,
        rrset: &SignedRRSet,
        current: &DomainName,
        chain: &Chain,
        proofs: &DenialProofs<'_>,
        fetches: &Fetches,
        now: u32,
    ) -> Result<Accepted, Failure> {
        // RFC 4035, 5.2: a DS RRset is authoritative data of the parent zone.
        let parent_chain;
        let zone = if rrset.rtype != ResourceType::DS || current.is_root() {
            chain
        } else {
            parent_chain = self.chain_for(&current.parent(), fetches, now)?;
            &parent_chain
        };
        match zone.status {
            SecurityStatus::Insecure => return Ok(Accepted::Insecure(zone.reason)),
            SecurityStatus::Bogus => return Ok(Accepted::Bogus(zone.reason)),
            SecurityStatus::Secure => {}
        }
        let Some(verified) = verify_rrset(rrset, &zone.zone, &zone.keys, now)? else {
            return Ok(Accepted::Bogus("RRset signature did not validate"));
        };
        // RFC 4035, 5.3.4. Authenticating a Wildcard Expanded RRset Positive Response.
        // it must take additional steps to verify the non-existence of an exact match or closer wildcard match
        // for the query.
        if let Some(wildcard_label_count) = verified.wildcard_label_count {
            if has_excessive_nsec3_iterations(proofs.rrsets) {
                return Ok(Accepted::Insecure(EXCESSIVE_NSEC3_ITERATIONS));
            }
            let Some(proof_ttl) = proofs.proves_next_closer_absent(current, wildcard_label_count) else {
                return Ok(Accepted::Bogus("Wildcard expansion is not proven"));
            };
            // RFC 9077, 4.1. A Note on Wildcards.
            // Validating resolvers consider an expanded wildcard valid for the wildcard's TTL, capped by the TTLs of
            // the NSEC or NSEC3 proof that shows that the wildcard expansion is legal.
            let mut records = verified.rrset.records;
            for record in &mut records {
                record.ttl = record.ttl.min(proof_ttl);
            }
            return Ok(Accepted::Records(records));
        }
        Ok(Accepted::Records(verified.rrset.records))
    }

    fn try_validate(
        &mut self,
        response: &Message,
        question: &Question,
        fetches: &Fetches,
        now: u32,
    ) -> Result<ValidationResult, Failure> {
        if !is_positive_or_name_error(response) {
            return Err(Failure::Error("DNS query failed"));
        }

        let bogus = |reason| ValidationResult {
            status: SecurityStatus::Bogus,
            records: Vec::new(),
            reason,
            denial_ttl: None,
        };

        let answers = group_rrsets(&response.answers);
        let authority = group_rrsets(&response.authorities);

        let mut result = ValidationResult {
            status: SecurityStatus::Secure,
            records: Vec::new(),
            reason: "",
            denial_ttl: None,
        };
        let mut current = question.name.clone();
        // Anything an alias leads to is valid for no longer than the alias.
        let mut chain_ttl = u32::MAX;
        let capped_to = |mut records: Vec<ResourceRecord>, chain_ttl: u32| {
            for record in &mut records {
                record.ttl = record.ttl.min(chain_ttl);
            }
            records
        };
        // The records validated so far, followed by the rest of the answer from `current` on, which could not be.
        let insecure = |verified: Vec<ResourceRecord>, current: &DomainName, chain_ttl: u32, reason| {
            let rest = response.answers_to(&Question {
                name: current.clone(),
                ..question.clone()
            });
            let mut records = verified;
            records.extend(capped_to(rest, chain_ttl));
            ValidationResult {
                status: SecurityStatus::Insecure,
                records,
                reason,
                denial_ttl: None,
            }
        };
        for _ in 0..=MAX_CNAME_CHAIN_LENGTH {
            let chain = self.chain_for(&current, fetches, now)?;
            match chain.status {
                SecurityStatus::Insecure => return Ok(insecure(result.records, &current, chain_ttl, chain.reason)),
                SecurityStatus::Bogus => return Ok(bogus(chain.reason)),
                SecurityStatus::Secure => {}
            }

            let mut answer = None;
            let mut cname = None;
            for rrset in &answers {
                if rrset.class != question.class || !rrset.owner.eq_ignore_case(&current) {
                    continue;
                }
                if rrset.rtype == question.rtype {
                    answer = Some(rrset);
                } else if rrset.rtype == ResourceType::CNAME {
                    cname = Some(rrset);
                }
            }

            // The NSEC3 iteration limit only applies where a denial proof is needed, which an exact answer is not.
            let denial_records = verify_denial_records(&authority, &chain.zone, &chain.keys, now)?;
            let proofs = DenialProofs {
                rrsets: &denial_records,
            };

            // RFC 6672, 3.1. CNAME Synthesis
            // The DNSSEC specification ([RFC4033] [RFC4034] [RFC4035]) says that the synthesized CNAME does not have to
            // be signed.  The signed DNAME has an RRSIG, and a validating resolver can check the CNAME against the
            // DNAME record and validate the signature over the DNAME RR.
            // NB: For a question for a CNAME, the synthesized CNAME is the answer.
            let alias = if question.rtype == ResourceType::CNAME {
                answer
            } else {
                cname
            };
            if let Some(cname) = alias.filter(|rrset| rrset.rtype == ResourceType::CNAME)
                && let Some(dname) = dname_above(&answers, &current, question.class)
            {
                let Some(target) = synthesized_target(cname, dname, &current) else {
                    return Ok(bogus("CNAME does not follow from the DNAME above it"));
                };
                let dname_records = match self.accept(dname, &dname.owner, &chain, &proofs, fetches, now)? {
                    Accepted::Records(records) => capped_to(records, chain_ttl),
                    Accepted::Insecure(reason) => return Ok(insecure(result.records, &current, chain_ttl, reason)),
                    Accepted::Bogus(reason) => return Ok(bogus(reason)),
                };
                // A CNAME RR with Time to Live (TTL) equal to the corresponding DNAME RR is synthesized
                chain_ttl = dname_records.iter().map(|record| record.ttl).fold(chain_ttl, u32::min);
                result.records.extend(dname_records);
                result.records.extend(capped_to(cname.records.clone(), chain_ttl));
                if question.rtype == ResourceType::CNAME {
                    return Ok(result);
                }
                current = target;
                continue;
            }

            if let Some(answer) = answer {
                return Ok(match self.accept(answer, &current, &chain, &proofs, fetches, now)? {
                    Accepted::Records(records) => {
                        result.records.extend(capped_to(records, chain_ttl));
                        result
                    }
                    Accepted::Insecure(reason) => insecure(result.records, &current, chain_ttl, reason),
                    Accepted::Bogus(reason) => bogus(reason),
                });
            }

            if let Some(cname) = cname
                && question.rtype != ResourceType::CNAME
            {
                match self.accept(cname, &current, &chain, &proofs, fetches, now)? {
                    Accepted::Records(records) => {
                        let records = capped_to(records, chain_ttl);
                        chain_ttl = records.iter().map(|record| record.ttl).fold(chain_ttl, u32::min);
                        result.records.extend(records);
                    }
                    Accepted::Insecure(reason) => return Ok(insecure(result.records, &current, chain_ttl, reason)),
                    Accepted::Bogus(reason) => return Ok(bogus(reason)),
                }
                let Some(RecordData::Cname(target)) = cname.records.first().map(|record| &record.data) else {
                    return Err(Failure::Error("CNAME record has no target"));
                };
                current = target.clone();
                continue;
            }

            if has_excessive_nsec3_iterations(&denial_records) {
                return Ok(insecure(
                    result.records,
                    &current,
                    chain_ttl,
                    EXCESSIVE_NSEC3_ITERATIONS,
                ));
            }
            let denial = if response.header.options.response_code() == ResponseCode::NAME_ERROR {
                proofs.proves_name_error(&current).ok_or("Name error is not proven")
            } else {
                proofs
                    .proves_no_data(&current, question.rtype)
                    .ok_or("Absence of the requested type is not proven")
            };
            let mut negative = match denial {
                Ok(Denial::Proven) => result,
                Ok(Denial::OptOut) => insecure(result.records, &current, chain_ttl, OPT_OUT),
                Err(reason) => return Ok(bogus(reason)),
            };
            negative.denial_ttl = proofs.ttl();
            return Ok(negative);
        }

        Ok(bogus("CNAME chain is too long"))
    }
}
