/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the style engine models of selectors beside their components: which pseudo-classes are
//! boolean element state facts, and where an `@scope` with no `<scope-start>` roots.

use super::input_types::StateFact;
use super::tree::StyleNodeID;
use crate::css::selector::PseudoClassType;

/// Where an `@scope` with no `<scope-start>` roots.
///
/// The specification's answer is the parent element of the sheet's owner node, which is an element
/// rather than a selector, so the caller resolves it. When the sheet has no owner node - a
/// constructed sheet - the answer is the shadow host or the root of the containing node tree, and
/// which of those holds is a property of the scope the sheet is asked in rather than of the sheet,
/// so it is left to matching.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImplicitScopeRoot {
    Node(StyleNodeID),
    ContainingTree,
}

// The pseudo-classes StyleEngine models as boolean element state facts.
//
// Returning `None` means the pseudo-class is an operator instead, not that it lacks a state fact.
include!(concat!(env!("OUT_DIR"), "/style_state_fact_compiler_generated.rs"));
