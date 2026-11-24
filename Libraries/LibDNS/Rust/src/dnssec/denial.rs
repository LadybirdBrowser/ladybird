/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cmp::Ordering;

use super::crypto::nsec3_hash;
use super::rrset::VerifiedRRSet;
use crate::message::{DomainName, Nsec, Nsec3, Nsec3HashAlgorithm, RecordData, ResourceType};

// RFC 9276, 3.2. Recommendation for Validating Resolvers.
// Validating resolvers MAY return an insecure response to their clients when processing NSEC3 records with
// iterations larger than 0.
pub(super) const MAX_NSEC3_ITERATIONS: u16 = 100;

// RFC 4648, 7. Base 32 Encoding with Extended Hex Alphabet.
fn base32hex_decode(input: &[u8]) -> Option<Vec<u8>> {
    let mut output = Vec::with_capacity(input.len() * 5 / 8);
    let mut bits: u32 = 0;
    let mut bit_count = 0;
    for &ch in input {
        let value = match ch {
            b'0'..=b'9' => ch - b'0',
            b'A'..=b'V' => ch - b'A' + 10,
            b'a'..=b'v' => ch - b'a' + 10,
            _ => return None,
        };
        bits = bits << 5 | u32::from(value);
        bit_count += 5;
        if bit_count >= 8 {
            output.push((bits >> (bit_count - 8)) as u8);
            bit_count -= 8;
        }
    }
    Some(output)
}

trait TypeBitMap {
    fn has_type(&self, rtype: ResourceType) -> bool;
}

impl TypeBitMap for Nsec {
    fn has_type(&self, rtype: ResourceType) -> bool {
        Nsec::has_type(self, rtype)
    }
}

impl TypeBitMap for Nsec3 {
    fn has_type(&self, rtype: ResourceType) -> bool {
        Nsec3::has_type(self, rtype)
    }
}

struct NsecRecord<'a> {
    owner: &'a DomainName,
    nsec: &'a Nsec,
    zone: &'a DomainName,
}

impl NsecRecord<'_> {
    fn matches(&self, name: &DomainName) -> bool {
        self.owner.eq_ignore_case(name)
    }

    // RFC 4035, 5.4. Authenticated Denial of Existence.
    // If the requested RR name would appear after an authenticated NSEC RR's owner name and before the name listed
    // in that NSEC RR's Next Domain Name field according to the canonical DNS name order defined in [RFC4034], then
    // no RRsets with the requested name exist in the zone.
    fn covers(&self, name: &DomainName) -> bool {
        if !self.zone.is_ancestor_or_equal_of(name) {
            return false;
        }
        let after_owner = self.owner.canonical_cmp(name) == Ordering::Less;
        let before_next = name.canonical_cmp(&self.nsec.next_domain_name) == Ordering::Less;
        // The last NSEC in the zone points back at the apex, which sorts before its owner.
        let is_last = self.nsec.next_domain_name.canonical_cmp(self.owner) != Ordering::Greater;
        after_owner && (before_next || is_last)
    }
}

struct Nsec3Record<'a> {
    owner_hash: Vec<u8>,
    nsec3: &'a Nsec3,
    zone: &'a DomainName,
}

impl Nsec3Record<'_> {
    fn matches(&self, name: &DomainName) -> bool {
        nsec3_hash(name, self.nsec3).is_some_and(|hash| hash == self.owner_hash)
    }

    fn covers(&self, name: &DomainName) -> bool {
        if !self.zone.is_ancestor_or_equal_of(name) {
            return false;
        }
        let Some(hash) = nsec3_hash(name, self.nsec3) else {
            return false;
        };
        let after_owner = self.owner_hash < hash;
        let before_next = hash < self.nsec3.next_hashed_owner_name;
        let is_last = self.nsec3.next_hashed_owner_name <= self.owner_hash;
        after_owner && (before_next || is_last)
    }
}

fn nsec_records(rrsets: &[VerifiedRRSet]) -> impl Iterator<Item = NsecRecord<'_>> {
    rrsets
        .iter()
        .filter(|verified| verified.rrset.rtype == ResourceType::NSEC)
        .flat_map(|verified| {
            verified
                .rrset
                .records
                .iter()
                .filter_map(move |record| match &record.data {
                    RecordData::Nsec(nsec) => Some(NsecRecord {
                        owner: &record.name,
                        nsec,
                        zone: &verified.zone,
                    }),
                    _ => None,
                })
        })
}

fn nsec3_records(rrsets: &[VerifiedRRSet]) -> impl Iterator<Item = Nsec3Record<'_>> {
    rrsets
        .iter()
        .filter(|verified| verified.rrset.rtype == ResourceType::NSEC3)
        .flat_map(|verified| {
            verified.rrset.records.iter().filter_map(move |record| {
                let RecordData::Nsec3(nsec3) = &record.data else {
                    return None;
                };
                // RFC 5155, 8.1 / 8.2: unknown hash types, unknown flags and excessive iterations are ignored.
                if nsec3.hash_algorithm != Nsec3HashAlgorithm::SHA1
                    || (nsec3.flags & !Nsec3::FLAG_OPT_OUT) != 0
                    || nsec3.iterations > MAX_NSEC3_ITERATIONS
                {
                    return None;
                }
                let first_label = record.name.labels.first()?;
                if !record.name.parent().eq_ignore_case(&verified.zone) {
                    return None;
                }
                let owner_hash = base32hex_decode(first_label)?;
                if owner_hash.len() != nsec3.next_hashed_owner_name.len() {
                    return None;
                }
                Some(Nsec3Record {
                    owner_hash,
                    nsec3,
                    zone: &verified.zone,
                })
            })
        })
}

// RFC 5155, 8.3. Closest Encloser Proof.
// In order to verify a closest encloser proof, the validator MUST find the longest name, X, such that
// o  X is an ancestor of QNAME that is matched by an NSEC3 RR present in the response.  This is a candidate for the
//    closest encloser, and
// o  The name one label longer than X (but still an ancestor of -- or equal to -- QNAME) is covered by an NSEC3 RR
//    present in the response.
struct ClosestEncloserProof {
    closest_encloser: DomainName,
    next_closer_opt_out: bool,
}

fn nsec3_closest_encloser_proof(rrsets: &[VerifiedRRSet], name: &DomainName) -> Option<ClosestEncloserProof> {
    let mut previous_covered = false;
    let mut previous_opt_out = false;
    for labels in (1..=name.labels.len()).rev() {
        let sname = name.suffix(labels);
        let mut covered = false;
        let mut opt_out = false;
        for record in nsec3_records(rrsets) {
            if record.matches(&sname) {
                // Once the closest encloser has been discovered, the validator MUST check that the NSEC3 RR that has
                // the closest encloser as the original owner name is from the proper zone.  The DNAME type bit must
                // not be set and the NS type bit may only be set if the SOA type bit is set.
                if !previous_covered
                    || record.nsec3.has_type(ResourceType::DNAME)
                    || (record.nsec3.has_type(ResourceType::NS) && !record.nsec3.has_type(ResourceType::SOA))
                {
                    return None;
                }
                return Some(ClosestEncloserProof {
                    closest_encloser: sname,
                    next_closer_opt_out: previous_opt_out,
                });
            }
            if record.covers(&sname) {
                covered = true;
                opt_out = record.nsec3.is_opt_out();
            }
        }
        previous_covered = covered;
        previous_opt_out = opt_out;
    }
    None
}

fn nsec_closest_encloser(rrsets: &[VerifiedRRSet], name: &DomainName) -> Option<DomainName> {
    // The closest encloser is the longest ancestor of the name that exists. Every ancestor of an NSEC's owner and
    // next name exists, so the longest suffix the name shares with either side of a covering NSEC is it.
    nsec_records(rrsets)
        .filter(|record| record.covers(name))
        .map(|record| {
            name.common_suffix_length(record.owner)
                .max(name.common_suffix_length(&record.nsec.next_domain_name))
        })
        .max()
        .map(|shared| name.suffix(shared))
}

pub(super) fn has_excessive_nsec3_iterations(rrsets: &[VerifiedRRSet]) -> bool {
    rrsets
        .iter()
        .filter(|verified| verified.rrset.rtype == ResourceType::NSEC3)
        .flat_map(|verified| &verified.rrset.records)
        .any(|record| matches!(&record.data, RecordData::Nsec3(nsec3) if nsec3.iterations > MAX_NSEC3_ITERATIONS))
}

pub(super) struct DenialProofs<'a> {
    pub rrsets: &'a [VerifiedRRSet],
}

impl DenialProofs<'_> {
    pub fn proves_name_error(&self, name: &DomainName) -> bool {
        // RFC 4035, 5.4. Authenticated Denial of Existence.
        // To prove the non-existence of an RRset, the resolver must be able to verify both that the queried RRset does
        // not exist and that no relevant wildcard RRset exists.
        if let Some(closest_encloser) = nsec_closest_encloser(self.rrsets, name)
            && self.proves_wildcard_absent(&closest_encloser)
        {
            return true;
        }

        // RFC 5155, 8.4. Validating Name Error Responses.
        // A validator MUST verify that there is a closest encloser proof for QNAME present in the response and that
        // there is an NSEC3 RR that covers the wildcard at the closest encloser.
        if let Some(proof) = nsec3_closest_encloser_proof(self.rrsets, name) {
            return self.proves_wildcard_absent(&proof.closest_encloser);
        }

        false
    }

    pub fn proves_wildcard_absent(&self, closest_encloser: &DomainName) -> bool {
        let wildcard = closest_encloser.with_label_prepended("*");
        nsec_records(self.rrsets).any(|record| record.covers(&wildcard))
            || nsec3_records(self.rrsets).any(|record| record.covers(&wildcard))
    }

    pub fn proves_next_closer_absent(&self, name: &DomainName, closest_encloser_label_count: usize) -> bool {
        // RFC 4035, 5.3.4 / RFC 5155, 8.8. Validating Wildcard Answer Responses.
        // Validators MUST verify that there is an NSEC3 RR that covers the "next closer" name to QNAME present in the
        // response.  This proves that QNAME itself did not exist and that the correct wildcard was used to generate the
        // response.
        if closest_encloser_label_count >= name.labels.len() {
            return false;
        }
        let next_closer = name.suffix(closest_encloser_label_count + 1);
        nsec_records(self.rrsets).any(|record| record.covers(&next_closer))
            || nsec3_records(self.rrsets).any(|record| record.covers(&next_closer))
    }

    pub fn proves_no_data(&self, name: &DomainName, rtype: ResourceType) -> bool {
        // RFC 4035, 5.4. Authenticated Denial of Existence.
        // If the requested RR name matches the owner name of an authenticated NSEC RR, then the NSEC RR's type bit map
        // field lists all RR types present at that owner name, and a resolver can prove that the requested RR type does
        // not exist by checking for the RR type in the bit map.
        // RFC 4035, 5.2: A security-aware resolver MUST use the parent NSEC RR when attempting to prove that a DS RRset
        // does not exist.
        let bitmap_denies = |record: &dyn TypeBitMap| {
            if record.has_type(rtype) || record.has_type(ResourceType::CNAME) {
                return false;
            }
            if rtype == ResourceType::DS && record.has_type(ResourceType::SOA) {
                return false;
            }
            true
        };

        if nsec_records(self.rrsets).any(|record| record.matches(name) && bitmap_denies(record.nsec)) {
            return true;
        }

        // RFC 5155, 8.5. Validating No Data Responses, QTYPE is not DS.
        // The validator MUST verify that an NSEC3 RR that matches QNAME is present and that both the QTYPE and the CNAME
        // type are not set in its Type Bit Maps field.
        if nsec3_records(self.rrsets).any(|record| record.matches(name) && bitmap_denies(record.nsec3)) {
            return true;
        }

        // Wildcard No Data: the name does not exist, but a wildcard does, and it lacks the type.
        // RFC 5155, 8.7. Validating Wildcard No Data Responses.
        let closest_encloser = nsec_closest_encloser(self.rrsets, name)
            .or_else(|| nsec3_closest_encloser_proof(self.rrsets, name).map(|proof| proof.closest_encloser));
        let Some(closest_encloser) = closest_encloser else {
            return false;
        };
        let wildcard = closest_encloser.with_label_prepended("*");
        nsec_records(self.rrsets).any(|record| record.matches(&wildcard) && bitmap_denies(record.nsec))
            || nsec3_records(self.rrsets).any(|record| record.matches(&wildcard) && bitmap_denies(record.nsec3))
    }

    pub fn proves_insecure_delegation(&self, name: &DomainName) -> bool {
        // RFC 4035, 5.2. Authenticating Referrals.
        // If the validator authenticates an NSEC RRset that proves that no DS RRset is present for this zone, then there
        // is no authentication path leading from the parent to the child.
        // RFC 5155, 8.9. Validating Referrals to Unsigned Subzones.
        // If there is an NSEC3 RR present in the response that matches the delegation name, then the validator MUST
        // ensure that the NS bit is set and that the DS bit is not set in the Type Bit Maps field of the NSEC3 RR.  The
        // validator MUST also ensure that the NSEC3 RR is from the correct (i.e., parent) zone.  This is done by
        // ensuring that the SOA bit is not set in the Type Bit Maps field of this NSEC3 RR.
        let is_unsigned_delegation = |record: &dyn TypeBitMap| {
            record.has_type(ResourceType::NS)
                && !record.has_type(ResourceType::DS)
                && !record.has_type(ResourceType::SOA)
        };

        if nsec_records(self.rrsets).any(|record| record.matches(name) && is_unsigned_delegation(record.nsec)) {
            return true;
        }
        if nsec3_records(self.rrsets).any(|record| record.matches(name) && is_unsigned_delegation(record.nsec3)) {
            return true;
        }

        // If there is no NSEC3 RR present that matches the delegation name, then the validator MUST verify a closest
        // provable encloser proof for the delegation name.  The validator MUST verify that the Opt-Out bit is set in the
        // NSEC3 RR that covers the "next closer" name to the delegation name.
        nsec3_closest_encloser_proof(self.rrsets, name).is_some_and(|proof| proof.next_closer_opt_out)
    }

    pub fn proves_name_exists_without_delegation(&self, name: &DomainName) -> bool {
        let is_not_a_cut =
            |record: &dyn TypeBitMap| !record.has_type(ResourceType::NS) && !record.has_type(ResourceType::DS);
        nsec_records(self.rrsets).any(|record| record.matches(name) && is_not_a_cut(record.nsec))
            || nsec3_records(self.rrsets).any(|record| record.matches(name) && is_not_a_cut(record.nsec3))
    }
}
