/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Style feature atoms and the local facts selector evaluation reads.
//!
//! An atom is a document-local `u32` standing for a tag/namespace pair, an ID, a class, an
//! attribute name, an attribute value, a namespace, or a state fact. C++ assigns the identities
//! because it owns the authoritative string payloads; Rust stores only the `u32` in bytecode,
//! postings, and input keys, and never a duplicate of the string. Interning a name on the C++
//! side is a hash lookup plus a reference count bump on a string that already exists, so a feature
//! comparison costs an integer compare and no conversion.
//!
//! Index keys are therefore semantic facts rather than strings. Substring and token attribute
//! operators drive from an attribute-name posting and run their exact value test in compound
//! bytecode instead of demanding a posting per substring, which is why a feature key names the
//! attribute and not the shape of the test applied to it. Only those tests need the value as text,
//! and only for the attributes an attached program actually mentions.

pub use super::facts_types::AttributeFact;
pub use super::facts_types::AttributeNameForms;
pub use super::facts_types::DispatchKey;
pub use super::facts_types::FeatureValue;
pub use super::facts_types::LocalFeatureKey;
pub use super::facts_types::StateSet;
pub use super::facts_types::StyleAtomID;
pub use super::facts_types::dispatch_bloom_bit;
