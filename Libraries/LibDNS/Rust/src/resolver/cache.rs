/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::message::{DomainName, RecordData, ResourceRecord, ResourceType};
use std::collections::{HashMap, HashSet};
use std::net::IpAddr;
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecurityStatus {
    Secure,
    Insecure,
}

/// A lookup snapshot; the cache keeps a separate, mutable copy.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LookupResult {
    pub name: DomainName,
    pub records: Vec<ResourceRecord>,
    /// Set when DNSSEC validation was attempted, with the result being Secure or Insecure.
    pub security_status: Option<SecurityStatus>,
}

impl LookupResult {
    pub fn addresses(&self) -> Vec<IpAddr> {
        self.records
            .iter()
            .filter_map(|record| match record.data {
                RecordData::A(address) => Some(IpAddr::V4(address)),
                RecordData::Aaaa(address) => Some(IpAddr::V6(address)),
                _ => None,
            })
            .collect()
    }

    pub fn has_addresses(&self) -> bool {
        self.records
            .iter()
            .any(|record| matches!(record.data, RecordData::A(_) | RecordData::Aaaa(_)))
    }
}

struct RecordWithExpiration {
    record: ResourceRecord,
    expiration: Instant,
}

pub(crate) struct CacheEntry {
    name: DomainName,
    valid: bool,
    request_done: bool,
    security_status: Option<SecurityStatus>,
    records: Vec<RecordWithExpiration>,
    queried_types: HashSet<ResourceType>,
}

impl CacheEntry {
    pub(crate) fn new(name: DomainName) -> Self {
        Self {
            name,
            valid: false,
            request_done: false,
            security_status: None,
            records: Vec::new(),
            queried_types: HashSet::new(),
        }
    }

    pub(crate) fn add_record(&mut self, record: ResourceRecord, received_at: Instant) {
        self.valid = true;
        let expiration = received_at + Duration::from_secs(u64::from(record.ttl));
        self.records.push(RecordWithExpiration { record, expiration });
    }

    pub(crate) fn will_add_record_of_type(&mut self, rtype: ResourceType) {
        self.queried_types.insert(rtype);
    }

    pub(crate) fn finished_request(&mut self) {
        self.request_done = true;
    }

    pub(crate) fn set_security_status(&mut self, status: SecurityStatus) {
        self.security_status = Some(status);
    }

    pub(crate) fn has_records(&self) -> bool {
        !self.records.is_empty()
    }

    fn has_record_of_type(&self, rtype: ResourceType) -> bool {
        self.records.iter().any(|record| record.record.rtype == rtype)
    }

    // Whether a completed lookup asked for this type, so having no records of it is a (negative) answer too.
    fn answers_type(&self, rtype: ResourceType) -> bool {
        self.has_record_of_type(rtype) || (self.request_done && self.queried_types.contains(&rtype))
    }

    pub(crate) fn expire(&mut self, now: Instant) {
        if !self.valid {
            return;
        }
        let mut expired_types = HashSet::new();
        self.records.retain(|record| {
            // RFC 1035, 3.2.1. Format
            // Zero values are interpreted to mean that the RR can only be used for the transaction in progress, and
            // should not be cached.
            if record.expiration <= now {
                expired_types.insert(record.record.rtype);
                return false;
            }
            true
        });

        // Forget the type when its last record expires, so expiry is not treated as a negative answer.
        for rtype in expired_types {
            if !self.has_record_of_type(rtype) {
                self.queried_types.remove(&rtype);
            }
        }

        if self.records.is_empty() && self.request_done {
            self.valid = false;
        }
    }

    fn can_be_removed(&self) -> bool {
        !self.valid && self.request_done
    }

    /// What a lookup for these types finds in the entry: records of those types, and the aliases and signatures that
    /// lead to them, as the lookup that filled the entry would have found for them.
    pub(crate) fn snapshot_for(&self, desired_types: &[ResourceType]) -> Arc<LookupResult> {
        let wanted = |rtype: ResourceType| {
            rtype == ResourceType::CNAME || desired_types.contains(&rtype) || desired_types.contains(&ResourceType::ANY)
        };
        let records = self
            .records
            .iter()
            .map(|record| &record.record)
            .filter(|record| match &record.data {
                RecordData::Rrsig(rrsig) if record.rtype == ResourceType::RRSIG => wanted(rrsig.type_covered),
                _ => wanted(record.rtype),
            })
            .cloned()
            .collect();
        Arc::new(LookupResult {
            name: self.name.clone(),
            records,
            security_status: self.security_status,
        })
    }

    pub(crate) fn snapshot(&self) -> Arc<LookupResult> {
        Arc::new(LookupResult {
            name: self.name.clone(),
            records: self.records.iter().map(|record| record.record.clone()).collect(),
            security_status: self.security_status,
        })
    }
}

pub(crate) struct Cache {
    entries: HashMap<String, CacheEntry>,
    max_entries: usize,
}

impl Cache {
    pub(crate) fn new(max_entries: usize) -> Self {
        Self {
            entries: HashMap::new(),
            max_entries,
        }
    }

    pub(crate) fn clear(&mut self) {
        self.entries.clear();
    }

    pub(crate) fn lookup(&mut self, name: &str, desired_types: &[ResourceType], now: Instant) -> Option<&CacheEntry> {
        let entry = self.entries.get_mut(name)?;
        entry.expire(now);
        if entry.can_be_removed() {
            return None;
        }
        if !desired_types.iter().all(|rtype| entry.answers_type(*rtype)) {
            return None;
        }
        Some(entry)
    }

    pub(crate) fn was_validated(entry: &CacheEntry) -> bool {
        entry.security_status.is_some()
    }

    pub(crate) fn insert(&mut self, name: String, entry: CacheEntry, now: Instant) {
        self.make_room(now);
        self.entries.insert(name, entry);
    }

    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }

    // Drops expired entries, then completed ones, until there is room for one more.
    fn make_room(&mut self, now: Instant) {
        if self.entries.len() < self.max_entries {
            return;
        }

        for entry in self.entries.values_mut() {
            entry.expire(now);
        }
        self.entries.retain(|_, entry| !entry.can_be_removed());
        if self.entries.len() < self.max_entries {
            return;
        }

        let excess = self.entries.len() + 1 - self.max_entries;
        let to_remove: Vec<String> = self
            .entries
            .iter()
            .filter(|(_, entry)| entry.request_done)
            .map(|(name, _)| name.clone())
            .take(excess)
            .collect();
        for name in to_remove {
            self.entries.remove(&name);
        }
    }
}
