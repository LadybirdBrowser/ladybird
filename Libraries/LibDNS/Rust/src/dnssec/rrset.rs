/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::Failure;
use super::crypto::{is_supported_algorithm, is_usable_zone_key, verify_signature};
use crate::message::{Class, Dnskey, DomainName, RecordData, ResourceRecord, ResourceType, Sig};

#[derive(Clone, Debug)]
pub(super) struct SignedRRSet {
    pub owner: DomainName,
    pub rtype: ResourceType,
    pub class: Class,
    pub records: Vec<ResourceRecord>,
    pub rrsigs: Vec<ReceivedRrsig>,
}

#[derive(Clone, Debug)]
pub(super) struct ReceivedRrsig {
    pub rrsig: Sig,
    /// The TTL of the RRSIG RR as received in the response.
    pub ttl: u32,
}

#[derive(Clone, Debug)]
pub(super) struct VerifiedRRSet {
    pub rrset: SignedRRSet,
    pub zone: DomainName,
    pub wildcard_label_count: Option<usize>,
    pub ttl: u32,
}

pub(super) fn group_rrsets(section: &[ResourceRecord]) -> Vec<SignedRRSet> {
    fn find<'a>(
        rrsets: &'a mut Vec<SignedRRSet>,
        owner: &DomainName,
        rtype: ResourceType,
        class: Class,
    ) -> &'a mut SignedRRSet {
        let index = match rrsets
            .iter()
            .position(|rrset| rrset.rtype == rtype && rrset.class == class && rrset.owner.eq_ignore_case(owner))
        {
            Some(index) => index,
            None => {
                rrsets.push(SignedRRSet {
                    owner: owner.clone(),
                    rtype,
                    class,
                    records: Vec::new(),
                    rrsigs: Vec::new(),
                });
                rrsets.len() - 1
            }
        };
        &mut rrsets[index]
    }

    let mut rrsets = Vec::new();
    for record in section {
        if record.rtype == ResourceType::RRSIG {
            let RecordData::Rrsig(rrsig) = &record.data else {
                continue;
            };
            find(&mut rrsets, &record.name, rrsig.type_covered, record.class)
                .rrsigs
                .push(ReceivedRrsig {
                    rrsig: rrsig.clone(),
                    ttl: record.ttl,
                });
            continue;
        }
        find(&mut rrsets, &record.name, record.rtype, record.class)
            .records
            .push(record.clone());
    }

    // Sets that only have signatures and no data are of no use.
    rrsets.retain(|rrset| !rrset.records.is_empty());
    rrsets
}

fn label_count_excluding_wildcard(name: &DomainName) -> usize {
    // RFC 4034, 3.1.3. The Labels Field.
    // The Labels field MUST be an unsigned integer between 1 and 255 inclusive, and does not count the null (root)
    // label nor a leading "*" label.
    if name.is_wildcard() {
        name.labels.len() - 1
    } else {
        name.labels.len()
    }
}

// RFC 4034, 3.1.5. Signature Expiration and Inception Fields.
// all comparisons involving these fields MUST use "Serial number arithmetic", as defined in [RFC1982].
// A distance of exactly 2^31 is undefined there, and counts as outside the validity period here.
fn is_within_validity_period(rrsig: &Sig, now: u32) -> bool {
    let until_expiration = rrsig.expiration.wrapping_sub(now) as i32;
    let since_inception = now.wrapping_sub(rrsig.inception) as i32;
    until_expiration >= 0 && since_inception >= 0
}

/// Tries every RRSIG of the set against `keys`, which must be the apex keys of `zone`.
pub(super) fn verify_rrset(
    rrset: &SignedRRSet,
    zone: &DomainName,
    keys: &[Dnskey],
    now: u32,
) -> Result<Option<VerifiedRRSet>, Failure> {
    for ReceivedRrsig { rrsig, ttl: rrsig_ttl } in &rrset.rrsigs {
        // RFC 4035, 5.3.1. Checking the RRSIG RR Validity.
        // o  The RRSIG RR's Signer's Name field MUST be the name of the zone that contains the RRset.
        // o  The RRSIG RR's Type Covered field MUST equal the RRset's type.
        // o  The number of labels in the RRset owner name MUST be greater than or equal to the value in the RRSIG
        //    RR's Labels field.
        // o  The validator's notion of the current time MUST be less than or equal to the time listed in the RRSIG
        //    RR's Expiration field.
        // o  The validator's notion of the current time MUST be greater than or equal to the time listed in the
        //    RRSIG RR's Inception field.
        if !rrsig.signers_name.eq_ignore_case(zone) || rrsig.type_covered != rrset.rtype {
            continue;
        }
        let owner_labels = label_count_excluding_wildcard(&rrset.owner);
        let rrsig_labels = usize::from(rrsig.label_count);
        if rrsig_labels > owner_labels {
            continue;
        }
        if !is_within_validity_period(rrsig, now) {
            continue;
        }
        if !is_supported_algorithm(rrsig.algorithm) {
            continue;
        }

        // RFC 4035, 5.3.2. Reconstructing the Signed Data.
        // if rrsig_labels < fqdn_labels, name = "*." | the rightmost rrsig_label labels of the fqdn
        let mut canonical_owner = rrset.owner.canonicalized();
        let mut wildcard_label_count = None;
        if rrsig_labels < owner_labels {
            canonical_owner = rrset
                .owner
                .suffix(rrsig_labels)
                .with_label_prepended("*")
                .canonicalized();
            wildcard_label_count = Some(rrsig_labels);
        }

        // RR(i) = name | type | class | OrigTTL | RDATA length | RDATA
        // The set of all RR(i) is sorted into canonical order.
        let owner_wire = canonical_owner
            .to_wire()
            .map_err(|_| Failure::Error("Owner name cannot be encoded"))?;
        let rdata_offset = owner_wire.len() + 2 + 2 + 4 + 2;
        let mut encoded_records: Vec<Vec<u8>> = Vec::with_capacity(rrset.records.len());
        for record in &rrset.records {
            let mut encoded = Vec::new();
            record
                .write_canonical_wire(&mut encoded, &canonical_owner, rrsig.original_ttl)
                .map_err(|_| Failure::Error("Record cannot be encoded"))?;
            if encoded_records.contains(&encoded) {
                continue;
            }
            encoded_records.push(encoded);
        }
        // RFC 4034, 6.3. Canonical RR Ordering within an RRset.
        // ... treating the RDATA portion of the canonical form of each RR as a left-justified unsigned octet sequence
        // in which the absence of an octet sorts before a zero octet.
        encoded_records.sort_by(|a, b| a[rdata_offset..].cmp(&b[rdata_offset..]));

        // signed_data = RRSIG_RDATA | RR(1) | RR(2)...
        let mut signed_data = Vec::new();
        let mut canonical_rrsig = rrsig.clone();
        canonical_rrsig.signers_name = rrsig.signers_name.canonicalized();
        canonical_rrsig
            .write_wire_excluding_signature(&mut signed_data)
            .map_err(|_| Failure::Error("RRSIG cannot be encoded"))?;
        for encoded in &encoded_records {
            signed_data.extend_from_slice(encoded);
        }

        // o  The RRSIG RR's Signer's Name, Algorithm, and Key Tag fields MUST match the owner name, algorithm, and
        //    key tag for some DNSKEY RR in the zone's apex DNSKEY RRset.
        // It is possible for more than one DNSKEY RR to match the conditions above. ... it MUST try each matching
        // DNSKEY RR until either the signature is validated or the validator has run out of matching public keys
        // to try.
        for key in keys {
            if key.algorithm != rrsig.algorithm || key.calculated_key_tag() != rrsig.key_tag || !is_usable_zone_key(key)
            {
                continue;
            }
            if !verify_signature(key, &signed_data, &rrsig.signature) {
                continue;
            }

            // RFC 4035, 5.3.3. Checking the Signature.
            // If the resolver accepts the RRset as authentic, the validator MUST set the TTL of the RRSIG RR and each
            // RR in the authenticated RRset to a value no greater than the minimum of:
            // o  the RRset's TTL as received in the response;
            // o  the RRSIG RR's TTL as received in the response;
            // o  the value in the RRSIG RR's Original TTL field; and
            // o  the difference of the RRSIG RR's Signature Expiration time and the current time.
            let mut ttl = rrsig.original_ttl.min(*rrsig_ttl);
            for record in &rrset.records {
                ttl = ttl.min(record.ttl);
            }
            ttl = ttl.min(rrsig.expiration.wrapping_sub(now));

            let mut verified = VerifiedRRSet {
                rrset: rrset.clone(),
                zone: zone.clone(),
                wildcard_label_count,
                ttl,
            };
            for record in &mut verified.rrset.records {
                record.ttl = record.ttl.min(ttl);
            }
            return Ok(Some(verified));
        }
    }

    Ok(None)
}

pub(super) fn verify_denial_records(
    authority: &[SignedRRSet],
    zone: &DomainName,
    keys: &[Dnskey],
    now: u32,
) -> Result<Vec<VerifiedRRSet>, Failure> {
    let mut verified = Vec::new();
    for rrset in authority {
        if rrset.rtype != ResourceType::NSEC && rrset.rtype != ResourceType::NSEC3 {
            continue;
        }
        if !zone.is_ancestor_or_equal_of(&rrset.owner) {
            continue;
        }
        if let Some(out) = verify_rrset(rrset, zone, keys, now)? {
            verified.push(out);
        }
    }
    Ok(verified)
}
