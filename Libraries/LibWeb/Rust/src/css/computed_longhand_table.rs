/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a style records beside its longhand values: whether each was important, inherited or
//! evaluated, the dependency flags, the metadata the host settles beside them, and the recorded
//! specified values helpers.

use std::ffi::c_void;

use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_is_inherited};
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

pub(crate) const LONGHAND_COUNT: usize = (LAST_LONGHAND_PROPERTY_ID - FIRST_LONGHAND_PROPERTY_ID + 1) as usize;

pub(crate) fn is_current_color_keyword(value: &StyleValueData) -> bool {
    matches!(value, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::keyword::CURRENTCOLOR)
}

pub(crate) const LONGHAND_BITMAP_BYTES: usize = LONGHAND_COUNT.div_ceil(8);

/// The effective value of one longhand: the value data pointer together with
/// which source produced it, so the C++ side can preserve wrapper identity
/// for overlay values and stamp style sheet context only onto table mints.
#[repr(C)]
pub struct FfiEffectiveLonghandValue {
    pub value: *const c_void,
    pub source: u8,
}

pub const EFFECTIVE_LONGHAND_SOURCE_TABLE: u8 = 0;
pub const EFFECTIVE_LONGHAND_SOURCE_OVERLAY: u8 = 1;
pub const EFFECTIVE_LONGHAND_SOURCE_SPECIFIED: u8 = 2;

/// The longhands that inherit by default, by slot.
const INHERITED_LONGHAND_BITS: [u8; LONGHAND_BITMAP_BYTES] = {
    use crate::css::property_metadata::{FIRST_INHERITED_PROPERTY_ID, LAST_INHERITED_PROPERTY_ID};
    let mut bits = [0; LONGHAND_BITMAP_BYTES];
    let mut property = FIRST_INHERITED_PROPERTY_ID;
    while property <= LAST_INHERITED_PROPERTY_ID {
        let index = (property - FIRST_LONGHAND_PROPERTY_ID) as usize;
        bits[index / 8] |= 1 << (index % 8);
        property += 1;
    }
    bits
};

/// The bit of `property_id` in a bitmap of longhands.
fn slot_index(property_id: u16) -> usize {
    assert!(
        (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property_id),
        "longhand bitmap indices are longhand property identifiers"
    );
    (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize
}

fn bitmap_bit(bits: &[u8; LONGHAND_BITMAP_BYTES], index: usize) -> bool {
    bits[index / 8] & (1 << (index % 8)) != 0
}

fn set_bitmap_bit(bits: &mut [u8; LONGHAND_BITMAP_BYTES], index: usize, value: bool) {
    if value {
        bits[index / 8] |= 1 << (index % 8);
    } else {
        bits[index / 8] &= !(1 << (index % 8));
    }
}

/// What a longhand table publishes beside its values, its recorded specified values, its zoom,
/// box display, color scheme and dependency flags: whether each longhand's value was important,
/// inherited or evaluated by the computation, and what the box adjustments and monospace
/// recascades read.
#[derive(Clone, PartialEq)]
pub(crate) struct LonghandTableFlags {
    important_bits: [u8; LONGHAND_BITMAP_BYTES],
    inherited_bits: [u8; LONGHAND_BITMAP_BYTES],
    evaluated_bits: [u8; LONGHAND_BITMAP_BYTES],
    overflow_before_adjustment: Option<[u16; 2]>,
    raw_cascaded_font_size: Option<RetainedStyleValueData>,
}

impl LonghandTableFlags {
    /// The flags of a copy of `initial` a drive starts from, before it computes anything.
    pub(crate) fn of_drive_copy(initial: &LonghandTableFlags) -> Self {
        Self {
            evaluated_bits: [0; LONGHAND_BITMAP_BYTES],
            ..initial.clone()
        }
    }

    /// The flags of the table `initial` seeds for a child, before the child computes anything:
    /// every longhand that inherits by default is inherited, and none is evaluated.
    pub(crate) fn of_child_seed(initial: &LonghandTableFlags) -> Self {
        Self {
            important_bits: initial.important_bits,
            inherited_bits: INHERITED_LONGHAND_BITS,
            evaluated_bits: [0; LONGHAND_BITMAP_BYTES],
            ..initial.clone()
        }
    }

    pub(crate) fn is_important(&self, property_id: u16) -> bool {
        bitmap_bit(&self.important_bits, slot_index(property_id))
    }

    /// One bit per longhand: whether its value is important.
    pub(crate) fn importance_bits(&self) -> &[u8] {
        &self.important_bits
    }

    pub(crate) fn is_inherited(&self, property_id: u16) -> bool {
        bitmap_bit(&self.inherited_bits, slot_index(property_id))
    }

    pub(crate) fn is_evaluated(&self, property_id: u16) -> bool {
        bitmap_bit(&self.evaluated_bits, slot_index(property_id))
    }

    pub(crate) fn set_important(&mut self, property_id: u16, important: bool) {
        set_bitmap_bit(&mut self.important_bits, slot_index(property_id), important);
    }

    pub(crate) fn set_inherited(&mut self, property_id: u16, inherited: bool) {
        set_bitmap_bit(&mut self.inherited_bits, slot_index(property_id), inherited);
    }

    /// The flags of a computation that starts from nothing: no longhand is important, inherited
    /// or evaluated.
    pub(crate) fn empty() -> Self {
        Self {
            important_bits: [0; LONGHAND_BITMAP_BYTES],
            inherited_bits: [0; LONGHAND_BITMAP_BYTES],
            evaluated_bits: [0; LONGHAND_BITMAP_BYTES],
            overflow_before_adjustment: None,
            raw_cascaded_font_size: None,
        }
    }

    pub(crate) fn mark_evaluated(&mut self, property_id: u16) {
        set_bitmap_bit(&mut self.evaluated_bits, slot_index(property_id), true);
    }

    /// Takes the importance and inheritance a drive found for each longhand it evaluated, one bit
    /// per longhand index in each of the words.
    pub(crate) fn merge_driver_flags(
        &mut self,
        important_words: &[u64],
        inherited_words: &[u64],
        evaluated_words: &[u64],
    ) {
        assert!(important_words.len() * 64 >= LONGHAND_COUNT);
        assert_eq!(inherited_words.len(), important_words.len());
        assert_eq!(evaluated_words.len(), important_words.len());
        for (word_index, &evaluated_word) in evaluated_words.iter().enumerate() {
            let mut remaining = evaluated_word;
            while remaining != 0 {
                let bit = remaining.trailing_zeros() as usize;
                remaining &= remaining - 1;
                let index = word_index * 64 + bit;
                if index >= LONGHAND_COUNT {
                    break;
                }
                set_bitmap_bit(
                    &mut self.important_bits,
                    index,
                    important_words[word_index] & (1 << bit) != 0,
                );
                set_bitmap_bit(
                    &mut self.inherited_bits,
                    index,
                    inherited_words[word_index] & (1 << bit) != 0,
                );
            }
        }
    }

    pub(crate) fn set_overflow_before_adjustment(&mut self, overflow: [u16; 2]) {
        self.overflow_before_adjustment = Some(overflow);
    }

    /// The overflow keywords before the box adjustments, where they adjusted them.
    pub(crate) fn overflow_before_adjustment(&self) -> Option<[u16; 2]> {
        self.overflow_before_adjustment
    }

    pub(crate) fn set_raw_cascaded_font_size(&mut self, value: Option<RetainedStyleValueData>) {
        self.raw_cascaded_font_size = value;
    }

    /// The element's cascaded font size, where it declares one, which the monospace recascade of
    /// a descendant reads.
    pub(crate) fn raw_cascaded_font_size_value(&self) -> Option<&StyleValueData> {
        self.raw_cascaded_font_size
            .as_ref()
            .and_then(RetainedStyleValueData::optional_data)
    }

    /// Whether these are the very flags `other` are: the same bits, and the same cascaded font size
    /// value, not only an equal one.
    pub(crate) fn same_as(&self, other: &Self) -> bool {
        self.important_bits == other.important_bits
            && self.inherited_bits == other.inherited_bits
            && self.evaluated_bits == other.evaluated_bits
            && self.overflow_before_adjustment == other.overflow_before_adjustment
            && self
                .raw_cascaded_font_size
                .as_ref()
                .map(RetainedStyleValueData::pointer)
                == other
                    .raw_cascaded_font_size
                    .as_ref()
                    .map(RetainedStyleValueData::pointer)
    }

    /// Whether these are the flags `other` are, as a publication compares them.
    pub(crate) fn publication_equals(&self, other: &Self) -> bool {
        use crate::css::style_value::optional_style_values_equal;
        self.important_bits == other.important_bits
            && self.inherited_bits == other.inherited_bits
            && self.evaluated_bits == other.evaluated_bits
            && self.overflow_before_adjustment == other.overflow_before_adjustment
            && optional_style_values_equal(
                self.raw_cascaded_font_size
                    .as_ref()
                    .and_then(RetainedStyleValueData::optional_data),
                other
                    .raw_cascaded_font_size
                    .as_ref()
                    .and_then(RetainedStyleValueData::optional_data),
            )
    }

    /// What these flags hold differently from `other`, by name, for a diagnostic to show.
    pub(crate) fn publication_differences(&self, other: &Self) -> Vec<String> {
        let bits_differ = |name: &str, first: &[u8; LONGHAND_BITMAP_BYTES], second: &[u8; LONGHAND_BITMAP_BYTES]| {
            let longhands: Vec<_> = (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID)
                .filter(|&property| {
                    let slot = slot_index(property);
                    bitmap_bit(first, slot) != bitmap_bit(second, slot)
                })
                .map(crate::css::property_metadata::property_name)
                .collect();
            (!longhands.is_empty()).then(|| format!("{name} of {}", longhands.join(", ")))
        };
        let mut differences: Vec<String> = [
            bits_differ("important bits", &self.important_bits, &other.important_bits),
            bits_differ("inherited bits", &self.inherited_bits, &other.inherited_bits),
            bits_differ("evaluated bits", &self.evaluated_bits, &other.evaluated_bits),
        ]
        .into_iter()
        .flatten()
        .collect();
        let mut note = |name: &str, first: String, second: String| {
            if first != second {
                differences.push(format!("{name} {first} != {second}"));
            }
        };
        note(
            "overflow before adjustment",
            format!("{:?}", self.overflow_before_adjustment),
            format!("{:?}", other.overflow_before_adjustment),
        );
        if !crate::css::style_value::optional_style_values_equal(
            self.raw_cascaded_font_size_value(),
            other.raw_cascaded_font_size_value(),
        ) {
            differences.push("cascaded font size".into());
        }
        differences
    }
}

/// The recorded specified value of `property` among `values`, where it has one.
fn inheritance_dependent_value_among<'a>(
    mut values: impl Iterator<Item = (u16, &'a RetainedStyleValueData)>,
    property: u16,
) -> Option<&'a RetainedStyleValueData> {
    values.find(|(held, _)| *held == property).map(|(_, value)| value)
}

/// Whether the recorded specified value of `property` moved from `old` to `new`, the recorded
/// specified values of two longhand tables or of the group values they are made from.
/// NB: Equal resolved colors can retain specified values that inherit differently.
fn inheritance_dependent_value_changed<'a, Values>(old: Values, new: Values, property: u16) -> bool
where
    Values: Iterator<Item = (u16, &'a RetainedStyleValueData)>,
{
    let old = inheritance_dependent_value_among(old, property);
    let new = inheritance_dependent_value_among(new, property);
    old.map(RetainedStyleValueData::pointer) != new.map(RetainedStyleValueData::pointer)
        && old.map(RetainedStyleValueData::data) != new.map(RetainedStyleValueData::data)
}

/// Whether a property that inherits by default retains another specified value among `new` than
/// among `old`, the recorded specified values of two longhand tables or of the group values they
/// are made from, which the children inheriting it resolve differently though the resolved colors
/// are equal.
pub(crate) fn inherited_inheritance_dependent_value_moved<'a, Values>(old: Values, new: Values) -> bool
where
    Values: Iterator<Item = (u16, &'a RetainedStyleValueData)> + Clone,
{
    old.clone().chain(new.clone()).any(|(property, _)| {
        property_is_inherited(property) && inheritance_dependent_value_changed(old.clone(), new.clone(), property)
    })
}

/// The non-inherited groups whose recorded specified values moved from `old` to `new`, the recorded
/// specified values of two longhand tables or of the group values they are made from.
/// NB: Report these source changes separately from resolved group payload changes.
pub(crate) fn non_inherited_inheritance_changes<'a, Values>(old: Values, new: Values) -> u32
where
    Values: Iterator<Item = (u16, &'a RetainedStyleValueData)> + Clone,
{
    // NB: A property both hold is visited once, through `old`.
    old.clone()
        .chain(
            new.clone()
                .filter(|(property, _)| inheritance_dependent_value_among(old.clone(), *property).is_none()),
        )
        .filter(|(property, _)| {
            !property_is_inherited(*property)
                && inheritance_dependent_value_changed(old.clone(), new.clone(), *property)
        })
        .fold(0, |groups, (property, _)| {
            groups
                | match crate::css::computed_values::StyleGroupMasks::get().dependencies(property) {
                    0 => u32::MAX,
                    mask => mask,
                }
        })
}

/// The dependency-flag bit of a record holding a value resolved against the viewport, as a `vw`
/// length is.
pub(crate) const DEPENDS_ON_VIEWPORT_METRICS: u8 = 1;
/// The dependency-flag bit of a record whose font metrics read the viewport, as a `vw` font size
/// does: what `em` and the other font-relative units resolve against moves with the viewport.
pub(crate) const FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS: u8 = 1 << 1;
/// The dependency-flag bit of a record whose element is `display: none` or inherits from one that is.
pub(crate) const IN_DISPLAY_NONE_SUBTREE: u8 = 1 << 2;
/// The dependency-flag bit of a highlight pseudo-element record whose `color` or `background-color`
/// comes from the author origin, on itself or up its highlight chain, so the paired default colors
/// do not apply. Bits 0 to 4 are the viewport, font-metric, display-none, swap-eligibility and image
/// flags.
/// https://drafts.csswg.org/css-pseudo-4/#paired-defaults
pub(crate) const HIGHLIGHT_COLORS_AUTHORED: u8 = 1 << 5;
/// The dependency-flag bit of a highlight pseudo-element record whose `color` is currentColor: the
/// computed value is the originating element's color, but painting draws the layer below instead.
/// https://drafts.csswg.org/css-pseudo-4/#highlight-text
pub(crate) const HIGHLIGHT_COLOR_IS_CURRENT_COLOR: u8 = 1 << 6;
/// The dependency-flag bit of a record with a cascaded value from the author origin for any property that disables
/// native appearance for widgets.
/// https://drafts.csswg.org/css-ui/#appearance-disabling-properties
pub(crate) const NATIVE_APPEARANCE_DISABLED: u8 = 1 << 7;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiComputedStyleMetadata {
    pub display_before_box_type_transformation: u32,
    pub effective_color_scheme: i16,
    pub dependency_flags: u8,
    pub in_display_none_subtree: bool,
}

#[cfg(test)]
mod tests {
    use crate::css::style_value_arc::StyleValueArc;

    use super::*;
    use crate::css::property_metadata::property_id;
    use crate::css::style_value::StyleValueData;

    type Recorded = Vec<(u16, RetainedStyleValueData)>;

    fn entry((property, value): &(u16, RetainedStyleValueData)) -> (u16, &RetainedStyleValueData) {
        (*property, value)
    }

    fn table_inheritance_changes(old: &Recorded, new: &Recorded) -> u32 {
        non_inherited_inheritance_changes(old.iter().map(entry), new.iter().map(entry))
    }

    #[test]
    fn inheritance_source_changes_keep_their_exact_non_inherited_group() {
        let current_color: Recorded = vec![(
            property_id::TEXT_DECORATION_COLOR,
            RetainedStyleValueData::from_owned(StyleValueData::Keyword {
                keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
            }),
        )];
        let equal: Recorded = vec![(
            property_id::TEXT_DECORATION_COLOR,
            RetainedStyleValueData::from_arc(StyleValueArc::new(StyleValueData::Keyword {
                keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
            })),
        )];
        assert_eq!(table_inheritance_changes(&current_color, &equal), 0);
        let resolved = Recorded::new();
        let expected = 1 << crate::css::table_group_builder::group_index::TEXT_RESET;
        assert_eq!(table_inheritance_changes(&current_color, &resolved), expected);
        assert_eq!(table_inheritance_changes(&resolved, &current_color), expected);
        assert_eq!(table_inheritance_changes(&current_color, &current_color.clone()), 0);
    }

    #[test]
    fn inherited_source_changes_do_not_mark_non_inherited_groups() {
        let current_color: Recorded = vec![(
            property_id::COLOR,
            RetainedStyleValueData::from_owned(StyleValueData::Keyword {
                keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
            }),
        )];
        assert_eq!(table_inheritance_changes(&current_color, &Recorded::new()), 0);
    }
}
