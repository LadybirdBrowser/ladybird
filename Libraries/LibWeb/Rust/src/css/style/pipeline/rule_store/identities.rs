/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The identities of the stylesheet program's sheets and rules, and what each rule version says.

pub use crate::css::cascaded_properties::CascadeOrigin;

use crate::css::declaration_block::DeclarationEntry;
use crate::css::style::cascade::CascadeOperator;
use crate::css::style::index::StyleAtomID;
use std::hash::Hash;

define_id! {
    /// Semantic identity of a sheet, stable across edits to its contents.
    pub struct SheetID(pub);
}
define_id! {
    /// Semantic identity of a rule, stable across edits to its declarations or selector.
    pub struct RuleID(pub);
}

impl RuleID {
    /// The bit telling a rule of the process's user agent rules from a rule of the document.
    const USER_AGENT: u32 = 1 << 31;

    /// The rule of the document that `rule`, a rule of the process's user agent rules, is to it.
    pub(in crate::css::style) fn of_user_agent(rule: RuleID) -> Self {
        assert!(rule.0 < Self::USER_AGENT, "user agent rule space exhausted");
        Self(rule.0 | Self::USER_AGENT)
    }

    /// The rule of the process's user agent rules this one is, if it is one.
    #[inline]
    pub(in crate::css::style) fn in_user_agent_rules(self) -> Option<RuleID> {
        (self.0 & Self::USER_AGENT != 0).then_some(Self(self.0 & !Self::USER_AGENT))
    }
}

/// One custom property a declaration block declares. The cascade tracks no winner per custom
/// property name: an element's custom-property environment cascades these by name when it is
/// computed, so a block carries them beside its longhand declarations.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CustomDeclaration {
    pub name: StyleAtomID,
    pub(crate) entry: DeclarationEntry,
}

const _: () = assert!(size_of::<CustomDeclaration>() == 8);

impl CustomDeclaration {
    pub(crate) fn new(name: StyleAtomID, important: bool, operator: CascadeOperator) -> Self {
        Self {
            name,
            entry: DeclarationEntry::new(crate::css::property_metadata::property_id::CUSTOM, important, operator),
        }
    }

    pub(crate) fn important(self) -> bool {
        self.entry.important()
    }

    pub(crate) fn operator(self) -> CascadeOperator {
        self.entry.operator()
    }
}

/// Immutable identity of a declaration block, whether it came from a rule or a style node. No
/// block's is zero, so an absent block costs nothing next to one.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct DeclarationBlockID(std::num::NonZeroU32);

impl DeclarationBlockID {
    /// The identity numbered `raw`, which is not zero.
    #[must_use]
    pub fn new(raw: u32) -> Self {
        Self(std::num::NonZeroU32::new(raw).expect("a declaration block identity is not zero"))
    }

    #[must_use]
    pub fn raw(self) -> u32 {
        self.0.get()
    }
}

/// Where the selector list of a rule starts among the selectors of its sheet, which the sheet's
/// rule store resolves, or [`Self::NONE`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SheetListStart(pub(super) u32);

impl SheetListStart {
    /// No list: the rule has no selectors, or an empty list.
    pub const NONE: Self = Self(u32::MAX);
}

define_id! {
    /// Identity of a cascade layer: the interned qualified layer name. Rules reference the identity
    /// rather than a copied numeric rank, so each tree scope can order the same name independently
    /// and a layer topology change does not rewrite every declaration that lives in it.
    pub struct CascadeLayerID(pub);
}

impl CascadeLayerID {
    /// The implicit outer layer that unlayered rules belong to.
    pub const UNLAYERED: Self = Self(0);
}

define_id! {
    /// Identity of an `@scope` scope. Distinct from a tree scope: one is a CSS construct, the other
    /// is a DOM one.
    pub struct StyleScopeID(pub);
}

impl StyleScopeID {
    /// Not inside any `@scope`.
    pub const NONE: Self = Self(0);
}

/// What kind of rule a program node is. The kinds exist so that a program delta can be routed to
/// the consumers that care: editing a keyframes rule begins at animations referencing its name,
/// not at every element in the scope.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RuleKind {
    Style,
    #[cfg(test)]
    Media,
    CounterStyle,
    FontFeatureValues,
    Keyframes,
    Property,
    Function,
}

impl RuleKind {
    /// Whether the rule contributes declarations through selector matching.
    #[must_use]
    pub fn matches_elements(self) -> bool {
        self == Self::Style
    }
}

/// The immutable semantic contents of one rule version.
///
/// The sheet order token is deliberately absent: rules reference their sheet, and the sheet holds
/// the token. Moving a sheet therefore updates one token instead of rewriting every rule in it.
///
/// A version holds its selector list by where it starts among the selectors of its sheet, which no
/// other list of the sheet ever starts at: a version whose selectors did not change keeps them.
#[derive(Clone, PartialEq, Eq, Hash, Debug)]
pub struct RuleVersion {
    pub kind: RuleKind,
    /// The name a rule declares, for the rules that reach their consumers by name rather than by
    /// matching anything: the animation a `@keyframes` describes, the custom property an `@property`
    /// registers, or `NONE`. Which index the name is looked up in is decided by the rule's kind.
    declared_name: StyleAtomID,
    pub selectors: SheetListStart,
    pub declaration_block: Option<DeclarationBlockID>,
    pub layer: CascadeLayerID,
    pub scope: StyleScopeID,
}

// NB: Every rule of every sheet a document attaches has one, at the rule's own number, which it
//     therefore does not name.
const _: () = assert!(size_of::<RuleVersion>() == 24);

impl RuleVersion {
    #[must_use]
    pub const fn new(kind: RuleKind) -> Self {
        Self {
            kind,
            declared_name: StyleAtomID::NONE,
            selectors: SheetListStart::NONE,
            declaration_block: None,
            layer: CascadeLayerID::UNLAYERED,
            scope: StyleScopeID::NONE,
        }
    }

    /// The name the rule declares, where it declares one.
    #[must_use]
    pub fn declared_name(&self) -> Option<StyleAtomID> {
        (!self.declared_name.is_none()).then_some(self.declared_name)
    }

    pub fn set_declared_name(&mut self, name: StyleAtomID) {
        self.declared_name = name;
    }
}
