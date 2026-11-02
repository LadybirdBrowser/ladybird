/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cascade priorities, and what a winning declaration is and where it comes from.
//!
//! The components of a priority, outermost first, follow the cascade sorting order:
//! origin and importance, encapsulation context, element-attached styles, layers, specificity,
//! scope proximity, and order of appearance. Importance reverses two of them - context and layers -
//! which is why importance is folded into the key when it is built rather than checked at every
//! comparison.

use std::hash::Hash;

use crate::css::cascaded_properties::CascadeOrigin;

use super::input_types::ElementDeclarationKind;
use super::pipeline::rule_store::RuleID;
use super::selector::Specificity;

/// Where an element-sourced declaration sits relative to the rules of its context.
///
/// Element-attached styles are a cascade component in their own right, above layers: a style
/// attribute beats every layered and unlayered rule in its context, whatever layer they are in.
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum ElementAttachment {
    /// A declaration from a stylesheet rule.
    Rule = 0,
    /// An HTML presentational hint or SVG presentation attribute mapped to declarations.
    PresentationalHint = 1,
    /// The `style` attribute.
    InlineStyle = 2,
}

/// The precedence ladder of origin and importance, lowest first.
///
/// Importance reverses the origin order, which is why this is one ladder rather than an origin plus
/// a flag: comparing an important user declaration against a normal author one is a comparison of
/// two rungs, not of two dimensions.
#[must_use]
pub fn origin_importance_rank(origin: CascadeOrigin, important: bool) -> u8 {
    match (origin, important) {
        (CascadeOrigin::UserAgent, false) => 0,
        (CascadeOrigin::User, false) => 1,
        (CascadeOrigin::AuthorPresentationalHint, false) => 2,
        (CascadeOrigin::Author, false) => 3,
        (CascadeOrigin::Author, true) => 5,
        (CascadeOrigin::AuthorPresentationalHint, true) => 6,
        (CascadeOrigin::User, true) => 7,
        (CascadeOrigin::UserAgent, true) => 8,
    }
}

/// The inputs a priority key is built from, before importance has been folded in.
#[derive(Clone, Copy)]
pub struct PriorityInputs {
    pub origin: CascadeOrigin,
    pub important: bool,
    /// Encapsulation depth: 0 for the document tree, increasing inwards.
    pub context_depth: u32,
    pub element_attachment: ElementAttachment,
    /// The layer's current rank, read from the layer order rather than copied into the rule.
    pub layer_rank: u64,
    pub specificity: Specificity,
    /// Distance from the `@scope` root; nearer wins. Zero when not scoped.
    pub scope_proximity: u32,
    /// The rule's position: its sheet's rank, then its own rank within that sheet.
    pub sheet_rank: u64,
    pub rule_rank: u64,
}

/// A total order over declarations. Greater wins.
///
/// The field order is the cascade sorting order, and `Ord` compares fields in declaration order, so
/// the comparison program is the struct layout.
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub struct CascadePriority {
    origin_importance: u8,
    /// Outer contexts win for normal declarations, inner contexts win for important ones.
    context: u32,
    element_attachment: ElementAttachment,
    /// Later layers win for normal declarations; the order is reversed for important ones.
    layer: u64,
    specificity: Specificity,
    /// Nearer scope roots win, so proximity is stored inverted.
    scope_proximity: u32,
    sheet_rank: u64,
    rule_rank: u64,
}

impl CascadePriority {
    #[must_use]
    pub fn new(inputs: PriorityInputs) -> Self {
        Self {
            origin_importance: origin_importance_rank(inputs.origin, inputs.important),
            context: if inputs.important {
                inputs.context_depth
            } else {
                u32::MAX - inputs.context_depth
            },
            element_attachment: inputs.element_attachment,
            layer: if inputs.important {
                u64::MAX - inputs.layer_rank
            } else {
                inputs.layer_rank
            },
            specificity: inputs.specificity,
            scope_proximity: u32::MAX - inputs.scope_proximity,
            sheet_rank: inputs.sheet_rank,
            rule_rank: inputs.rule_rank,
        }
    }

    /// Whether a `revert` or `revert-layer` declared at this priority rolls back past a declaration
    /// at `other`, so that the declaration takes no part in what the property reverts to.
    #[must_use]
    pub fn reverts_past(&self, operator: CascadeOperator, other: &Self) -> bool {
        // UA, user, and author.
        let origin_group = |origin_importance: u8| match origin_importance {
            0 | 8 => 0,
            1 | 7 => 1,
            _ => 2,
        };
        match operator {
            // https://drafts.csswg.org/css-cascade-5/#valdef-all-revert
            CascadeOperator::Revert => origin_group(other.origin_importance) >= origin_group(self.origin_importance),
            // https://drafts.csswg.org/css-cascade-5/#valdef-all-revert-layer
            // The cascaded value rolls back to the earlier layers of the declaration's origin and
            // context, important or not.
            CascadeOperator::RevertLayer => {
                // NB: Inline styles occupy a separate cascade step, even though they share the implicit outer
                //     layer's rank with unlayered style rules, so a revert-layer in them only rolls back other
                //     inline styles.
                other.origin() == self.origin()
                    && other.context_depth() == self.context_depth()
                    && match self.element_attachment {
                        ElementAttachment::InlineStyle => other.element_attachment == ElementAttachment::InlineStyle,
                        _ => other.layer_rank() >= self.layer_rank(),
                    }
            }
            _ => false,
        }
    }

    /// The origin of the declaration this priority was built for.
    #[must_use]
    pub fn cascade_origin(&self) -> CascadeOrigin {
        match self.origin_importance {
            0 | 8 => CascadeOrigin::UserAgent,
            1 | 7 => CascadeOrigin::User,
            2 | 6 => CascadeOrigin::AuthorPresentationalHint,
            3 | 5 => CascadeOrigin::Author,
            rank => unreachable!("origin-importance rank {rank} names no origin"),
        }
    }

    /// The origin the priority was built from, as its rank among normal declarations.
    fn origin(&self) -> u8 {
        match self.is_important() {
            true => 8 - self.origin_importance,
            false => self.origin_importance,
        }
    }

    fn is_important(&self) -> bool {
        self.origin_importance > origin_importance_rank(CascadeOrigin::Author, false)
    }

    /// The encapsulation depth the priority was built from.
    fn context_depth(&self) -> u32 {
        match self.is_important() {
            true => self.context,
            false => u32::MAX - self.context,
        }
    }

    /// The rank of the layer the priority was built from.
    fn layer_rank(&self) -> u64 {
        match self.is_important() {
            true => u64::MAX - self.layer,
            false => self.layer,
        }
    }
}

// -- Winners and stopping keys -----------------------------------------------------------------

/// A CSS-wide keyword acts as a cascade operator rather than an eagerly flattened value, so its
/// dependency on parent style, origin, and layer topology stays explicit and repairable.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u8)]
pub enum CascadeOperator {
    /// An ordinary declared value.
    #[default]
    Declared,
    Inherit,
    Initial,
    Unset,
    /// Resumes below the current origin.
    Revert,
    /// Resumes below the current layer in the applicable origin and importance ordering.
    RevertLayer,
}

impl CascadeOperator {
    /// The operator of the largest number.
    pub(crate) const LAST: Self = Self::RevertLayer;

    /// The operator `bits` numbers, which is one.
    pub(crate) fn from_bits(bits: u8) -> Self {
        match bits {
            0 => Self::Declared,
            1 => Self::Inherit,
            2 => Self::Initial,
            3 => Self::Unset,
            4 => Self::Revert,
            5 => Self::RevertLayer,
            _ => unreachable!("not a cascade operator"),
        }
    }
}

/// The winning declaration for one property of one style node.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum WinnerSource {
    Rule(RuleID),
    Element(ElementDeclarationKind),
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inputs(origin: CascadeOrigin, important: bool) -> PriorityInputs {
        PriorityInputs {
            origin,
            important,
            context_depth: 0,
            element_attachment: ElementAttachment::Rule,
            layer_rank: 100,
            specificity: Specificity::default(),
            scope_proximity: 0,
            sheet_rank: 10,
            rule_rank: 10,
        }
    }

    #[test]
    fn the_origin_ladder_puts_importance_where_the_cascade_does() {
        let normal_ua = CascadePriority::new(inputs(CascadeOrigin::UserAgent, false));
        let normal_user = CascadePriority::new(inputs(CascadeOrigin::User, false));
        let hint = CascadePriority::new(inputs(CascadeOrigin::AuthorPresentationalHint, false));
        let normal_author = CascadePriority::new(inputs(CascadeOrigin::Author, false));
        let important_author = CascadePriority::new(inputs(CascadeOrigin::Author, true));
        let important_user = CascadePriority::new(inputs(CascadeOrigin::User, true));
        let important_ua = CascadePriority::new(inputs(CascadeOrigin::UserAgent, true));

        let ladder = [
            normal_ua,
            normal_user,
            hint,
            normal_author,
            important_author,
            important_user,
            important_ua,
        ];
        assert!(ladder.windows(2).all(|pair| pair[0] < pair[1]));

        // A presentational hint loses to an author rule, however specific the hint's source.
        assert!(hint < normal_author);
    }

    #[test]
    fn importance_reverses_layer_order() {
        let mut early = inputs(CascadeOrigin::Author, false);
        early.layer_rank = 10;
        let mut late = inputs(CascadeOrigin::Author, false);
        late.layer_rank = 20;
        assert!(
            CascadePriority::new(early) < CascadePriority::new(late),
            "later layers win"
        );

        early.important = true;
        late.important = true;
        assert!(
            CascadePriority::new(late) < CascadePriority::new(early),
            "and the order reverses for important declarations"
        );
    }

    #[test]
    fn importance_reverses_encapsulation_context() {
        let mut outer = inputs(CascadeOrigin::Author, false);
        outer.context_depth = 0;
        let mut inner = inputs(CascadeOrigin::Author, false);
        inner.context_depth = 2;
        assert!(
            CascadePriority::new(inner) < CascadePriority::new(outer),
            "outer contexts win for normal declarations"
        );

        outer.important = true;
        inner.important = true;
        assert!(
            CascadePriority::new(outer) < CascadePriority::new(inner),
            "inner contexts win for important ones"
        );
    }

    #[test]
    fn an_element_attached_style_beats_every_rule_in_its_context() {
        let mut rule = inputs(CascadeOrigin::Author, false);
        rule.layer_rank = u64::MAX - 1;
        rule.specificity = Specificity {
            ids: 100,
            classes: 100,
            types: 100,
        };
        let mut inline = inputs(CascadeOrigin::Author, false);
        inline.element_attachment = ElementAttachment::InlineStyle;
        inline.layer_rank = 0;

        assert!(CascadePriority::new(rule) < CascadePriority::new(inline));
    }

    #[test]
    fn specificity_then_proximity_then_source_order_break_ties() {
        let base = inputs(CascadeOrigin::Author, false);
        let mut specific = base;
        specific.specificity = Specificity {
            classes: 1,
            ..Specificity::default()
        };
        assert!(CascadePriority::new(base) < CascadePriority::new(specific));

        let mut near = base;
        near.scope_proximity = 1;
        let mut far = base;
        far.scope_proximity = 5;
        assert!(
            CascadePriority::new(far) < CascadePriority::new(near),
            "nearer scopes win"
        );

        let mut early = base;
        early.rule_rank = 10;
        let mut late = base;
        late.rule_rank = 20;
        assert!(CascadePriority::new(early) < CascadePriority::new(late));

        let mut early_sheet = base;
        early_sheet.sheet_rank = 1;
        early_sheet.rule_rank = u64::MAX;
        let mut late_sheet = base;
        late_sheet.sheet_rank = 2;
        late_sheet.rule_rank = 0;
        assert!(
            CascadePriority::new(early_sheet) < CascadePriority::new(late_sheet),
            "sheet order outranks position within a sheet"
        );
    }

    #[test]
    fn importance_is_weighed_per_declaration_not_per_rule() {
        // Two author rules, the less specific one marking its declaration important. Ordering the
        // rules once by specificity would let the more specific normal declaration win; weighing
        // each declaration with its own importance puts the important one on a higher rung.
        let mut specific = inputs(CascadeOrigin::Author, false);
        specific.specificity = Specificity {
            ids: 1,
            ..Specificity::default()
        };
        specific.rule_rank = 20;

        let mut important = inputs(CascadeOrigin::Author, true);
        important.specificity = Specificity {
            classes: 1,
            ..Specificity::default()
        };
        important.rule_rank = 10;

        assert!(
            CascadePriority::new(specific) < CascadePriority::new(important),
            "an important author declaration outranks a more specific normal one"
        );

        // The same rule's other declaration, written without `!important`, does not.
        let mut normal = important;
        normal.important = false;
        assert!(
            CascadePriority::new(normal) < CascadePriority::new(specific),
            "and the same rule's normal declaration still loses to the more specific one"
        );
    }
}
