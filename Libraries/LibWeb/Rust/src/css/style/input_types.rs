/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Typed semantic inputs, and the element inputs a transaction takes beside the projection's
//! changes. Typing matters: treating every change as a generic version bump would move work from
//! mutation time into an unnecessarily broad lazy validation later, which is exactly the
//! amplification this engine exists to avoid.

use std::collections::BTreeMap;
use std::collections::BTreeSet;

#[cfg(test)]
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::instrumentation::Counter;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;

/// The sides of a transaction that its readers compare.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum TransactionFactSide {
    Before,
    After,
}

// -- Typed semantic inputs -------------------------------------------------------------------

// An element or document state fact.
//
// States are semantic facts rather than bespoke invalidation entry points, so a transition
// publishes an old and a new value and only operators depending on that state receive it.
//
// Every boolean pseudo-class the parser can produce has an entry here. That exhaustiveness is the
// point: a pseudo-class with no fact would compile to something the engine cannot route, and a
// selector that cannot be routed cannot be invalidated. Facts whose value is a parameter rather
// than a boolean - `:dir()`, `:lang()`, `:state()`, `:heading()` - are operators instead, because
// a bitset cannot carry their argument.
include!(concat!(env!("OUT_DIR"), "/style_state_fact_generated.rs"));

impl StateFact {
    /// Whether the elements in this state are enumerated by a feature posting.
    ///
    /// A state a user interaction puts an element in holds on a handful of elements at a time: the
    /// focused element and the ancestors focus is within, the hovered chain, the activated chain,
    /// the top layer. A subject compound carrying nothing but such a state names its subjects as
    /// well as an ID does, so a change elsewhere in the selector reaches those subjects rather than
    /// every element its combinators could lead to. A state an element's nature decides, such as
    /// being a link or enabled, holds on most of a document and is left to the compound's names.
    #[must_use]
    pub fn has_selector_posting(self) -> bool {
        matches!(
            self,
            Self::Active
                | Self::Focus
                | Self::FocusVisible
                | Self::FocusWithin
                | Self::Fullscreen
                | Self::Hover
                | Self::Modal
                | Self::PictureInPicture
                | Self::PopoverOpen
                | Self::Target
        )
    }
}

/// Declarations sourced from one style node rather than a stylesheet rule. Each kind keeps its
/// language-defined cascade placement: presentational hints do not masquerade as inline style, and
/// none of them enter the rule index merely because their source syntax is an attribute.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u8)]
pub enum ElementDeclarationKind {
    /// The `style` attribute.
    InlineStyle,
    /// An HTML presentational-hint attribute.
    PresentationalHint,
    /// An SVG presentation attribute such as `fill` or `stroke`.
    SvgPresentationAttribute,
}

impl ElementDeclarationKind {
    pub const COUNT: usize = Self::SvgPresentationAttribute as usize + 1;
    pub const ALL: [Self; Self::COUNT] = [
        Self::InlineStyle,
        Self::PresentationalHint,
        Self::SvgPresentationAttribute,
    ];

    #[must_use]
    pub const fn index(self) -> usize {
        self as usize
    }
}

/// The tree relations of one style node, as of one side of a mutation.
///
/// This is the minimum old-side information invalidation needs. It is not a
/// traversable old DOM and does not copy unchanged descendant fields: old-side facts exist to
/// establish a complete impact region, not to reconstruct history.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TreeRelations {
    pub parent: Option<StyleNodeID>,
    pub previous_element_sibling: Option<StyleNodeID>,
    pub next_element_sibling: Option<StyleNodeID>,
    pub tree_scope: TreeScopeID,
    pub assigned_slot: Option<StyleNodeID>,
}

impl TreeRelations {
    /// Relations of a node with no parent, slot, or host: the shape a node has before insertion and
    /// after removal.
    #[must_use]
    #[cfg(test)]
    pub fn detached(tree_scope: TreeScopeID) -> Self {
        Self {
            parent: None,
            previous_element_sibling: None,
            next_element_sibling: None,
            tree_scope,
            assigned_slot: None,
        }
    }
}

/// An input the host publishes: a style-node identity plus the typed fact or relation it describes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum InputKey {
    TreeRelations(StyleNodeID),
    LocalFeature(StyleNodeID, LocalFeatureKey),
    State(StyleNodeID, StateFact),
    /// A non-selector style input owned by the element changed. This is an edge-triggered action,
    /// not retained fact state; its value says only which reaction is pending. A test records one directly.
    #[cfg(test)]
    ElementStyleInput(StyleNodeID),
}

pub const STYLE_REACTION_PUBLISHED_STYLE: u8 = 1 << 0;
pub const STYLE_REACTION_RECOMPUTE_STYLE: u8 = 1 << 1;
pub const STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES: u8 = 1 << 4;
pub const STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED: u8 = 1 << 6;
/// The element's font environment moved: a face its font cascade names became available, failed
/// or changed. The `@font-face` table that decides the cascade is published to the engine and
/// versioned by the font-environment generation. The element computes again from the rules it
/// matched, which no font decides: alone, this asks for no rematch, unlike `STYLE_REACTION_RECOMPUTE_STYLE`.
pub const STYLE_REACTION_FONT_INPUTS_CHANGED: u8 = 1 << 7;

impl InputKey {
    /// What the engine counts an input of this key as.
    #[must_use]
    #[cfg_attr(
        not(test),
        expect(
            clippy::unnecessary_wraps,
            reason = "only a test records an element style input, which counts nothing"
        )
    )]
    pub fn counter(self) -> Option<Counter> {
        match self {
            Self::TreeRelations(..) => Some(Counter::TreeDeltas),
            Self::LocalFeature(..) => Some(Counter::LocalFeatureDeltas),
            Self::State(..) => Some(Counter::StateDeltas),
            #[cfg(test)]
            Self::ElementStyleInput(..) => None,
        }
    }
}

/// The new value of an input a test records directly.
#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputValue {
    Feature(FeatureValue),
    ElementStyleInput { reaction: u8, inherited_style_groups: u8 },
}

/// An element style input's pending reaction, and the inherited style groups it names.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ElementStyleInput {
    pub reaction: u8,
    pub inherited_style_groups: u8,
}

/// Parts of the document's environment an element's style may compute from beside its matches,
/// its parent and the document's published inputs: what a transaction says moved of them, and
/// what a record says its computation read of them.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct StyleEnvironment(u8);

impl StyleEnvironment {
    /// The used color scheme: the user's preferred one and the schemes the document supports,
    /// which `color-scheme`, `light-dark()` and the system colors resolve against.
    pub const COLOR_SCHEME: Self = Self(1 << 0);
    /// The device pixel ratio, which widths snapped as border widths and `image-set()` read.
    pub const DEVICE_PIXEL_RATIO: Self = Self(1 << 1);
    /// The media features a value's `if()` or custom function may test.
    pub const MEDIA_FEATURES: Self = Self(1 << 2);
    /// The viewport's width, which viewport-relative lengths may resolve against.
    pub const VIEWPORT_WIDTH: Self = Self(1 << 3);
    /// The viewport's height.
    pub const VIEWPORT_HEIGHT: Self = Self(1 << 4);
    /// Both axes of the viewport.
    pub const VIEWPORT: Self = Self(Self::VIEWPORT_WIDTH.0 | Self::VIEWPORT_HEIGHT.0);
    /// Every part.
    pub const ALL: Self =
        Self(Self::COLOR_SCHEME.0 | Self::DEVICE_PIXEL_RATIO.0 | Self::MEDIA_FEATURES.0 | Self::VIEWPORT.0);

    /// The parts the host names by `bits`, as `StyleEnvironmentChange` bits.
    pub fn from_bits(bits: u8) -> Self {
        Self(bits & Self::ALL.0)
    }

    #[must_use]
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Whether this and `other` share a part.
    #[must_use]
    pub fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }

    /// The parts this and `other` share.
    #[must_use]
    pub fn intersection(self, other: Self) -> Self {
        Self(self.0 & other.0)
    }
}

impl std::ops::BitOr for StyleEnvironment {
    type Output = Self;
    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

impl std::ops::BitOrAssign for StyleEnvironment {
    fn bitor_assign(&mut self, other: Self) {
        self.0 |= other.0;
    }
}

/// The inputs the host published since the last transaction that the projection holds no row
/// for: which elements' own declarations moved, which elements owe a style reaction, what moved
/// of the document's environment, and whether something moved that can restyle any element. The
/// projection says every change to the elements' facts and relations itself.
#[derive(Clone, Default)]
pub struct PendingElementInputs {
    declarations: BTreeSet<StyleNodeID>,
    style_inputs: BTreeMap<StyleNodeID, ElementStyleInput>,
    environment: StyleEnvironment,
    restyles_everything: bool,
}

impl PendingElementInputs {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.declarations.is_empty()
            && self.style_inputs.is_empty()
            && self.environment.is_empty()
            && !self.restyles_everything
    }

    /// Whether the next transaction restyles elements for something of the document's environment
    /// that moved, or every element.
    #[must_use]
    pub fn moves_environment(&self) -> bool {
        !self.environment.is_empty() || self.restyles_everything
    }

    /// Note that `moved` of the document's environment moved, which restyles the elements whose
    /// records read it in the next transaction.
    pub fn note_environment_moved(&mut self, moved: StyleEnvironment) {
        self.environment |= moved;
    }

    /// Restyle every element in the next transaction: an input moved that can reach any of them.
    pub fn restyle_everything(&mut self) {
        self.restyles_everything = true;
    }

    pub fn note_declaration(&mut self, node: StyleNodeID) {
        self.declarations.insert(node);
    }

    /// Note a reaction requested for an element, beside every other one requested for it since
    /// the last transaction.
    pub fn note_style_input(&mut self, node: StyleNodeID, input: ElementStyleInput) {
        let pending = self.style_inputs.entry(node).or_default();
        pending.reaction |= input.reaction;
        pending.inherited_style_groups |= input.inherited_style_groups;
    }

    /// The elements an input is pending for.
    pub fn nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.declarations.iter().chain(self.style_inputs.keys()).copied()
    }

    /// Take the pending inputs into a transaction, in element order.
    pub fn take_transaction(&mut self) -> StyleUpdate {
        let Self {
            declarations,
            style_inputs,
            environment,
            restyles_everything,
        } = std::mem::take(self);
        StyleUpdate {
            declarations: declarations.into_iter().collect(),
            style_inputs: style_inputs.into_iter().collect(),
            environment,
            restyles_everything,
            rule_changes: Default::default(),
        }
    }
}

/// What one transaction takes beside the projection's changes, in element order.
#[derive(Default)]
#[must_use]
pub struct StyleUpdate {
    /// The elements whose own declarations moved.
    pub declarations: Vec<StyleNodeID>,
    /// The elements that owe a style reaction, each with every reaction requested for it.
    pub style_inputs: Vec<(StyleNodeID, ElementStyleInput)>,
    /// What moved of the document's environment, which restyles the elements reading it.
    pub environment: StyleEnvironment,
    /// Whether the transaction restyles every element, from the facts as they are.
    pub restyles_everything: bool,
    /// What the transaction changed of the rules.
    pub(crate) rule_changes: super::pipeline::rule_store::RuleChanges,
}

impl StyleUpdate {
    /// Release the transaction's scratch charge. Called once the transaction has been consumed.
    pub fn release(self) {}

    /// Whether the transaction names no element and restyles nothing by itself.
    #[must_use]
    pub fn has_no_element_inputs(&self) -> bool {
        self.declarations.is_empty()
            && self.style_inputs.is_empty()
            && self.environment.is_empty()
            && !self.restyles_everything
    }
}
