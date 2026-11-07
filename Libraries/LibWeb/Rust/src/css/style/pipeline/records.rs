/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The computed styles a style pass produced, by element, which the elements after them compute
//! against. Each element's record is written once, by whichever thread styles it, before any of its
//! children is styled.

use super::super::animations::DeclarationScope;
use super::super::container_queries::{ContainerRecords, ContainerVerdict};
use super::super::input_types::TransactionFactSide;
use super::super::selector_evaluation::StructuralRead;
use super::super::*;
use super::drive::DriveRecords;
use super::traversal::InitialStyle;
use super::write_once::WriteOnce;
use crate::css::computed_values::{GroupSeed, GroupValueFacts, GroupValueLayout, LonghandID};
use crate::css::style::installed_records::InstalledRecordID;
use crate::css::style::memory_report::{MemoryOwner, MemoryReport};
use crate::css::style_compute::RootFontReads;
use crate::css::table_group_builder::group_index;
use std::borrow::Cow;
use std::sync::Arc;

/// One element's computed style: its groups, each holding one reference, with their computed values,
/// the facts its readers read without them, and what its computation recorded beside them.
#[repr(C)]
pub(crate) struct Record {
    /// NB: First, so that a record's address is the address of the groups the host and layout read
    ///     through their places.
    pub(in super::super) payloads: crate::css::computed_values::StyleGroups,
    /// What the record's readers read of the element's style without its groups, and the identity
    /// of the slot holding the record.
    pub(super) header: RecordHeader,
}

/// A record's header word, which the host reads from its own thread: the facts of the record,
/// settled when it is made, the computed `display` in bits 0-12 and the flags in bits 16-23; whether
/// its groups hold images in bit 15, which bit 14 says is settled, by the thread computing it or the
/// slot taking it; and the identity of that slot, its index in bits 24-47 and its generation in bits
/// 48-63, which the slot names as it takes the record and forgets as it lets go of it.
pub(in super::super) struct RecordHeader(std::sync::atomic::AtomicU64);

const _: () = assert!((InstalledRecordID::MAX_SLOT as u64) < RecordHeader::SLOT_MASK);
const _: () = assert!((InstalledRecordID::MAX_GENERATION as u64) <= u64::MAX >> RecordHeader::GENERATION_SHIFT);

impl RecordHeader {
    const DISPLAY_MASK: u64 = (1 << 13) - 1;
    const IMAGE_VALUES_SETTLED: u64 = 1 << 14;
    const HOLDS_IMAGE_VALUES: u64 = 1 << 15;
    const FLAGS_SHIFT: u32 = 16;
    const SLOT_SHIFT: u32 = 24;
    const GENERATION_SHIFT: u32 = 48;
    const SLOT_MASK: u64 = (1 << 24) - 1;
    /// The slot bits of a record no slot holds.
    const NO_SLOT: u64 = Self::SLOT_MASK;
    const IDENTITY_MASK: u64 = !((1 << Self::SLOT_SHIFT) - 1);

    pub(super) fn new(facts: RecordFacts) -> Self {
        Self(std::sync::atomic::AtomicU64::new(
            u64::from(facts.display) | u64::from(facts.flags) << Self::FLAGS_SHIFT | Self::NO_SLOT << Self::SLOT_SHIFT,
        ))
    }

    fn facts(&self) -> RecordFacts {
        let header = self.load();
        RecordFacts {
            display: (header & Self::DISPLAY_MASK) as u16,
            flags: (header >> Self::FLAGS_SHIFT) as u8,
        }
    }

    fn load(&self) -> u64 {
        self.0.load(std::sync::atomic::Ordering::Relaxed)
    }

    fn holds_image_values(&self) -> bool {
        self.load() & Self::HOLDS_IMAGE_VALUES != 0
    }

    /// Replaces the bits of `mask` with `bits` in one step: the host reads the word from its own thread, and must
    /// never see an identity no slot named.
    fn replace(&self, mask: u64, bits: u64) {
        use std::sync::atomic::Ordering::Relaxed;
        let _ = self
            .0
            .fetch_update(Relaxed, Relaxed, |header| Some((header & !mask) | bits));
    }

    fn set_holds_image_values(&self, holds_image_values: bool) {
        self.replace(
            Self::IMAGE_VALUES_SETTLED | Self::HOLDS_IMAGE_VALUES,
            Self::IMAGE_VALUES_SETTLED
                | if holds_image_values {
                    Self::HOLDS_IMAGE_VALUES
                } else {
                    0
                },
        );
    }

    /// Whether the groups of the record hold images, where that is settled.
    fn settled_holds_image_values(&self) -> Option<bool> {
        let header = self.load();
        (header & Self::IMAGE_VALUES_SETTLED != 0).then_some(header & Self::HOLDS_IMAGE_VALUES != 0)
    }

    fn installed_record(&self) -> Option<InstalledRecordID> {
        let header = self.load();
        match (header >> Self::SLOT_SHIFT) & Self::SLOT_MASK {
            Self::NO_SLOT => None,
            slot => Some(InstalledRecordID::from_parts(
                slot as u32,
                (header >> Self::GENERATION_SHIFT) as u32,
            )),
        }
    }

    /// Names the slot `id` where no slot is named yet, and answers whether it did: a fork of the
    /// render state may be naming a slot of its own for the same record on its own thread.
    fn name_installed_record(&self, id: InstalledRecordID) -> bool {
        use std::sync::atomic::Ordering::Relaxed;
        assert!(id.slot() <= InstalledRecordID::MAX_SLOT && id.generation() <= InstalledRecordID::MAX_GENERATION);
        let identity = u64::from(id.slot()) << Self::SLOT_SHIFT | u64::from(id.generation()) << Self::GENERATION_SHIFT;
        self.0
            .fetch_update(Relaxed, Relaxed, |header| {
                ((header >> Self::SLOT_SHIFT) & Self::SLOT_MASK == Self::NO_SLOT)
                    .then_some((header & !Self::IDENTITY_MASK) | identity)
            })
            .is_ok()
    }

    fn forget_installed_record(&self) {
        self.replace(Self::IDENTITY_MASK, Self::NO_SLOT << Self::SLOT_SHIFT);
    }
}

/// The pseudo-element of a kind, or the `::highlight()` of a custom highlight name.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(in super::super) struct PseudoElementTarget {
    pub(in super::super) kind: u8,
    pub(in super::super) highlight_name: StyleAtomID,
}

impl PseudoElementTarget {
    pub(in super::super) const fn of_kind(kind: u8) -> Self {
        Self {
            kind,
            highlight_name: StyleAtomID::NONE,
        }
    }

    pub(in super::super) const fn custom_highlight(name: StyleAtomID) -> Self {
        Self {
            kind: super::super::publication::pseudo_kind::HIGHLIGHT,
            highlight_name: name,
        }
    }

    pub(in super::super) fn is_custom_highlight(self) -> bool {
        !self.highlight_name.is_none()
    }
}

/// The pseudo-elements of an element that generate a box, as a bit per kind.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub(in super::super) struct PseudoElementStyles(u16);

impl PseudoElementStyles {
    pub(in super::super) const NONE: Self = Self(0);

    /// The pseudo-elements `records` style, by kind.
    pub(in super::super) fn of(records: &[(PseudoElementTarget, Arc<Record>)]) -> Self {
        Self(
            records
                .iter()
                .filter(|(target, _)| !target.is_custom_highlight())
                .fold(0, |mask, (target, _)| mask | (1 << target.kind)),
        )
    }

    /// The pseudo-elements of the kinds whose bits `bits` holds, as a test names them.
    #[cfg(test)]
    pub(in super::super) fn from_bits(bits: u64) -> Self {
        Self(u16::try_from(bits).expect("every pseudo-element kind has a bit of 16"))
    }

    /// The bits of the kinds, `1 << kind` each, as the host reads them.
    pub(in super::super) fn bits(self) -> u64 {
        u64::from(self.0)
    }
}

/// The custom properties the children of an element inherit.
#[derive(Clone)]
pub(in super::super) enum InheritableCustomProperties {
    /// Those the element holds, as they are where no registration keeps one from inheriting.
    Held,
    /// Others: those it holds but for the ones a registration keeps from inheriting, or those its
    /// parent handed down where it holds none that inherit.
    Other(Option<Arc<crate::css::custom_properties::CustomPropertyStore>>),
}

impl InheritableCustomProperties {
    /// The custom properties children inherit as `inheritable`, of an element holding `held`.
    pub(in super::super) fn of(
        held: Option<&Arc<crate::css::custom_properties::CustomPropertyStore>>,
        inheritable: Option<Cow<'_, Arc<crate::css::custom_properties::CustomPropertyStore>>>,
    ) -> Self {
        match (held, &inheritable) {
            (None, None) => Self::Held,
            (Some(held), Some(inheritable)) if Arc::ptr_eq(held, inheritable) => Self::Held,
            _ => Self::Other(inheritable.map(Cow::into_owned)),
        }
    }

    fn same_as(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Held, Self::Held) => true,
            (Self::Other(None), Self::Other(None)) => true,
            (Self::Other(Some(mine)), Self::Other(Some(theirs))) => Arc::ptr_eq(mine, theirs),
            _ => false,
        }
    }
}

/// Whether both are the very same allocation, or neither is any.
fn same_arc<T: ?Sized>(first: Option<&Arc<T>>, second: Option<&Arc<T>>) -> bool {
    match (first, second) {
        (None, None) => true,
        (Some(first), Some(second)) => Arc::ptr_eq(first, second),
        _ => false,
    }
}

/// The custom declarations an element's cascade decided, as written.
pub(crate) type CustomDeclarations = Arc<[(CustomDeclaration, RetainedStyleValueData)]>;

/// What a record's rare set holds beside its groups.
#[derive(Clone, Default)]
pub(crate) struct RareSetFacts {
    /// What the element's computation recorded and read beyond its values, which the records of
    /// elements styled alike share.
    pub(crate) facts: Option<Arc<RareStyleFacts>>,
    /// The custom declarations the element's cascade decided, as written, which a sample of its
    /// animations resolves the ones reading an animated custom property from.
    pub(crate) custom_declarations: Option<CustomDeclarations>,
}

impl RareSetFacts {
    /// Whether `other` holds these facts and declarations themselves.
    pub(crate) fn is(&self, other: &Self) -> bool {
        same_arc(self.facts.as_ref(), other.facts.as_ref())
            && same_arc(self.custom_declarations.as_ref(), other.custom_declarations.as_ref())
    }
}

/// What an element's computation read beyond its inherited context and its declarations.
#[derive(Clone, Copy, Default, PartialEq)]
pub(crate) struct ComputationReads {
    /// The style groups of the properties that do not inherit whose parent's values the element's
    /// computation read, or every group where one of them belongs to none.
    pub(in crate::css::style) parent_reset_groups: u32,
    /// Whether the element's computation read its place among its siblings, through a
    /// tree-counting function.
    pub(in crate::css::style) reads_sibling_position: bool,
    /// How much of the root element's font the element's computation resolved lengths against.
    pub(in crate::css::style) reads_root_font: RootFontReads,
    /// Whether the element's computation resolved a length against a metric of its own or its
    /// parent's first available font.
    pub(in crate::css::style) reads_font_metric_lengths: bool,
    /// What the element's computation read of the document's environment.
    pub(in crate::css::style) environment_reads: crate::css::style::input_types::StyleEnvironment,
    /// Whether the element's computation read its attributes, through `attr()`, which what reads
    /// them beside the winners' values settles as the record is made.
    pub(in crate::css::style) reads_attributes: bool,
    /// The container-relative units the element's computation resolved against its query
    /// containers, as a `container_relative_length_unit_mask`.
    pub(in crate::css::style) container_units: u8,
    /// Whether the element's `display` computed to `none` only through the custom properties its
    /// animations sampled, and so to something else when ignoring them.
    pub(in crate::css::style) display_none_is_animated: bool,
}

/// What a style records of its computation beyond its values: the flags beside its values, what
/// its groups are built from, where its longhands live, and what its computation read. Elements
/// styled alike record the same, so siblings, and parents and their children, share one.
#[derive(Clone)]
pub(crate) struct RareStyleFacts {
    /// The custom properties the element's children inherit.
    pub(in super::super) inheritable_custom_properties: InheritableCustomProperties,
    /// The flags a longhand table of the element publishes beside its values, which elements
    /// whose declarations and computations wrote the same longhands share.
    pub(in super::super) table_flags: Arc<crate::css::computed_longhand_table::LonghandTableFlags>,
    /// What the groups are built from beside their computed values.
    pub(in super::super) value_facts: GroupValueFacts,
    /// Where the record's computed longhand values live.
    pub(in super::super) longhand_home: LonghandHome,
    /// The custom properties the element's longhands substitute, which a sample of its animations
    /// moving none of them leaves them as they are.
    pub(in super::super) custom_property_reads: crate::css::style::custom_property_cascade::LonghandCustomPropertyReads,
    /// What else the element's computation read.
    pub(in super::super) reads: ComputationReads,
    /// The identity of the counter-style registry of its style scope, where the style reads it,
    /// or zero.
    pub(in super::super) counter_style_environment_identity: u64,
    /// The scope the winning `animation-name` was declared in, where the `@keyframes` it names are
    /// looked for first.
    pub(in super::super) animation_name_scope: DeclarationScope,
    /// The pseudo-elements of the element that generate a box, which a pass settles as it styles
    /// them beside the element.
    // NB: Records styled alike but for these hold the same style, so `same_as` leaves them out.
    pub(in super::super) pseudo_element_styles: PseudoElementStyles,
}

// NB: Every element with a style of its own holds one of these; a field that grows it grows them all.
const _: () = assert!(size_of::<Record>() == 64);
const _: () = assert!(std::mem::offset_of!(Record, payloads) == 0);
const _: () = assert!(std::mem::offset_of!(Record, header) == 56);
const _: () = assert!(size_of::<RecordFacts>() == 4);
const _: () = assert!(size_of::<GroupValueFacts>() == 32);
const _: () = assert!(size_of::<RareStyleFacts>() == 120);

impl RareStyleFacts {
    /// These facts, with `inheritable` as the custom properties the element's children inherit.
    pub(in super::super) fn with_inheritable_custom_properties(
        self: &Arc<Self>,
        inheritable: InheritableCustomProperties,
    ) -> Arc<Self> {
        if self.inheritable_custom_properties.same_as(&inheritable) {
            return self.clone();
        }
        Arc::new(Self {
            inheritable_custom_properties: inheritable,
            ..(**self).clone()
        })
    }

    /// These facts, styling the pseudo-elements of `styles`.
    pub(in super::super) fn with_pseudo_element_styles(self: &Arc<Self>, styles: PseudoElementStyles) -> Arc<Self> {
        if self.pseudo_element_styles == styles {
            return self.clone();
        }
        Arc::new(Self {
            pseudo_element_styles: styles,
            ..(**self).clone()
        })
    }

    /// The facts of a style a computation made, which holds its longhands in its groups, and whose
    /// pseudo-elements a pass settles after it.
    pub(in super::super) fn of_computation(
        inheritable_custom_properties: InheritableCustomProperties,
        counter_style_environment_identity: u64,
        table_flags: Arc<crate::css::computed_longhand_table::LonghandTableFlags>,
        value_facts: GroupValueFacts,
        custom_property_reads: crate::css::style::custom_property_cascade::LonghandCustomPropertyReads,
        reads: ComputationReads,
        animation_name_scope: DeclarationScope,
    ) -> Self {
        Self {
            inheritable_custom_properties,
            table_flags,
            value_facts,
            longhand_home: LonghandHome::Groups,
            custom_property_reads,
            reads,
            counter_style_environment_identity,
            animation_name_scope,
            pseudo_element_styles: PseudoElementStyles::NONE,
        }
    }

    /// The facts of a style the host made, which read nothing of the document: `source_groups` holds
    /// its longhands.
    fn of_host_style(
        table_flags: crate::css::computed_longhand_table::LonghandTableFlags,
        value_facts: crate::css::computed_values::GroupValueFacts,
        source_groups: crate::css::computed_values::StyleGroups,
        counter_style_environment_identity: u64,
    ) -> Self {
        Self {
            inheritable_custom_properties: InheritableCustomProperties::Held,
            table_flags: Arc::new(table_flags),
            value_facts,
            longhand_home: LonghandHome::SourceGroups(DerivedSource::of(source_groups)),
            custom_property_reads: Default::default(),
            reads: ComputationReads::default(),
            counter_style_environment_identity,
            animation_name_scope: DeclarationScope::Unscoped,
            pseudo_element_styles: PseudoElementStyles::NONE,
        }
    }

    /// Whether `other` records what these do, so that a record may hold either.
    pub(in super::super) fn same_as(&self, other: &RareStyleFacts) -> bool {
        matches!(
            (&self.longhand_home, &other.longhand_home),
            (LonghandHome::Groups, LonghandHome::Groups)
        ) && self
            .inheritable_custom_properties
            .same_as(&other.inheritable_custom_properties)
            && (Arc::ptr_eq(&self.table_flags, &other.table_flags) || self.table_flags.same_as(&other.table_flags))
            && self.value_facts.publication_equals(&other.value_facts)
            && self.custom_property_reads == other.custom_property_reads
            && self.reads == other.reads
            && self.counter_style_environment_identity == other.counter_style_environment_identity
            && self.animation_name_scope == other.animation_name_scope
    }
}

/// Where a record's computed longhand values live.
#[derive(Clone)]
pub(in super::super) enum LonghandHome {
    /// In the record's own groups.
    Groups,
    /// In the groups of the style a derived record was derived from, each of which it holds a
    /// reference to: a derived style holds the longhands of its source.
    SourceGroups(Box<DerivedSource>),
}

/// What a derived record holds of the style it was derived from.
#[derive(Clone)]
pub(in super::super) struct DerivedSource {
    /// The source's groups, which hold the derived record's longhands.
    pub(in super::super) groups: crate::css::computed_values::StyleGroups,
    /// What the animations of the element sampled over the source, where the derived record is a
    /// sample of them.
    pub(in super::super) sample: Option<Box<Sample>>,
}

impl DerivedSource {
    fn of(groups: crate::css::computed_values::StyleGroups) -> Box<Self> {
        Box::new(Self { groups, sample: None })
    }
}

/// What a record of a sample of the animations of an element holds beside its groups.
#[derive(Clone)]
pub(in super::super) struct Sample {
    /// The installed record the animations sample over.
    pub(in super::super) base: Arc<Record>,
    /// The values the animations compose over the base.
    pub(in super::super) animated_overlay: Arc<crate::css::animated_overlay::AnimatedOverlay>,
    /// What a view of the sample depends on: what a view of the base does, with an `<image>` a
    /// sampled value holds where the base holds none, and in the display:none subtree a sampled
    /// display puts it in.
    pub(in super::super) dependency_flags: u8,
    /// The identity the host names the sample by.
    pub(in super::super) identity: computed::FinalStyleRecordID,
}

/// What a record holds of an element's style beside its values, settled when the record is made.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub(in super::super) struct RecordFacts {
    /// The computed `display`, packed by `pack_display`.
    display: u16,
    /// The dependency flags at their bit values, with whether the element is `display: none` or
    /// inherits from one that is at `IN_DISPLAY_NONE_SUBTREE`, and `DECLARES_CSS_ANIMATIONS` and
    /// `DECLARES_TRANSITIONS`.
    flags: u8,
}

/// `display` in 16 bits: its tag in bits 0-1, then the three bytes of its encoding in five, five
/// and one bit.
fn pack_display(display: crate::css::display::FfiDisplay) -> u16 {
    let [tag, first, second, third] = display.encoded().to_le_bytes();
    assert!(
        tag < 4 && first < 32 && second < 32 && third < 2,
        "every display value packs into 16 bits"
    );
    u16::from(tag) | u16::from(first) << 2 | u16::from(second) << 7 | u16::from(third) << 12
}

fn unpack_display(packed: u16) -> crate::css::display::FfiDisplay {
    let field = |shift: u16, bits: u16| u32::from((packed >> shift) & ((1 << bits) - 1));
    crate::css::display::FfiDisplay::from_raw(field(0, 2) | field(2, 5) << 8 | field(7, 5) << 16 | field(12, 1) << 24)
}

impl RecordFacts {
    /// Whether the computed `animation-name` names any animation.
    const DECLARES_CSS_ANIMATIONS: u8 = 1 << 3;
    /// Whether the computed `transition-*` values make any longhand transitionable.
    const DECLARES_TRANSITIONS: u8 = 1 << 4;

    /// The facts of a style whose computed values `value_of` answers, which resolves no color yet.
    pub(in super::super) fn of_values<'v>(
        value_of: impl Fn(u16) -> &'v RetainedStyleValueData,
        dependency_flags: u8,
        in_display_none_subtree: bool,
    ) -> Self {
        use crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE;
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::{
            animation_names_declare_css_animations, display_of, transition_values_declare_transitions,
        };
        let mut facts = Self {
            display: pack_display(display_of(value_of(prop::DISPLAY))),
            flags: 0,
        };
        facts.add_dependency_flags(dependency_flags);
        facts.flags |= (u8::from(in_display_none_subtree) * IN_DISPLAY_NONE_SUBTREE)
            | (u8::from(animation_names_declare_css_animations(value_of(prop::ANIMATION_NAME)))
                * Self::DECLARES_CSS_ANIMATIONS)
            | (u8::from(transition_values_declare_transitions(
                value_of(prop::TRANSITION_PROPERTY),
                value_of(prop::TRANSITION_DURATION),
                value_of(prop::TRANSITION_DELAY),
            )) * Self::DECLARES_TRANSITIONS);
        facts
    }

    /// The computed `display`.
    pub(in super::super) fn display(&self) -> crate::css::display::FfiDisplay {
        unpack_display(self.display)
    }

    /// The dependency flags of the element's computation, without whether it is in a
    /// `display: none` subtree.
    pub(in super::super) fn dependency_flags(&self) -> u8 {
        use crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE;
        self.flags & !(IN_DISPLAY_NONE_SUBTREE | Self::DECLARES_CSS_ANIMATIONS | Self::DECLARES_TRANSITIONS)
    }

    /// Adds `flags` to the dependency flags of the element's computation.
    pub(in super::super) fn add_dependency_flags(&mut self, flags: u8) {
        assert_eq!(
            flags & (Self::DECLARES_CSS_ANIMATIONS | Self::DECLARES_TRANSITIONS),
            0,
            "a computation's dependency flags leave the declaration bits clear"
        );
        self.flags |= flags;
    }

    /// Whether the element is `display: none` or inherits from one that is.
    pub(in super::super) fn in_display_none_subtree(&self) -> bool {
        self.flags & crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE != 0
    }

    /// Whether the computed `animation-name` names any animation.
    pub(in super::super) fn declares_css_animations(&self) -> bool {
        self.flags & Self::DECLARES_CSS_ANIMATIONS != 0
    }

    /// Whether the computed `transition-*` values make any longhand transitionable.
    pub(in super::super) fn declares_transitions(&self) -> bool {
        self.flags & Self::DECLARES_TRANSITIONS != 0
    }

    /// The dependency flags the element's style publishes.
    pub(in super::super) fn publication_dependency_flags(&self) -> u8 {
        self.flags & !(Self::DECLARES_CSS_ANIMATIONS | Self::DECLARES_TRANSITIONS)
    }
}

/// Which children of an element its new record restyles.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum ChildImpact {
    None,
    /// Only the children that read the element's values of properties that do not inherit.
    ResetReaders,
    /// Only the custom properties it hands down moved: each child reads them anew, and computes
    /// again where it substitutes one that moved.
    CustomProperties,
    /// Both of the above.
    ResetReadersAndCustomProperties,
    All,
}

// NB: The inherited groups are the inherited sets, which a child holding its parents set shares.
const _: () = assert!(
    crate::css::computed_values::INHERITED_SET_GROUPS.len()
        == crate::css::style::computed::ENGINE_INHERITED_GROUP_COUNT
);

impl ChildImpact {
    /// Which children of an element computing `new` where it computed `old` compute anew, where
    /// they inherited `inherited`.
    pub(super) fn of_records(new: &Record, old: &Record, inherited: &crate::css::computed_values::StyleGroups) -> Self {
        // An inherited longhand no group holds reaches the children through the table alone.
        let ungrouped_inherited_moved = || {
            crate::css::computed_values::StyleGroupMasks::get()
                .ungrouped_inherited()
                .iter()
                .any(|&property| {
                    match (
                        computed::ViewLonghands::of_record(new).handle(property),
                        computed::ViewLonghands::of_record(old).handle(property),
                    ) {
                        (Some(new), Some(old)) => !crate::css::style_value::retained_style_values_equal(new, old),
                        (new, old) => new.is_some() != old.is_some(),
                    }
                })
        };
        if new.payloads.current_color() != old.payloads.current_color()
            || new.payloads.inherited_values() != old.payloads.inherited_values()
            // https://drafts.csswg.org/css-color-adjust-1/#color-scheme-prop
            // A child whose `color-scheme` is the one it inherits uses the scheme its parent uses.
            || new.effective_color_scheme() != old.effective_color_scheme()
            || new.facts().publication_dependency_flags() != old.facts().publication_dependency_flags()
            || new.facts().display != old.facts().display
            || ungrouped_inherited_moved()
            // https://drafts.csswg.org/css-color-4/#resolving-other-colors
            // A child inheriting a value that reads `currentcolor` resolves it against its own
            // color, so the specified values the record retains reach the children where the
            // colors they resolve to here did not move.
            || crate::css::computed_longhand_table::inherited_inheritance_dependent_value_moved(
                old.inheritance_dependent_values(),
                new.inheritance_dependent_values(),
            )
        {
            return ChildImpact::All;
        }
        let impact = match ChildImpact::of_groups(&new.payloads, inherited) {
            ChildImpact::None
                if crate::css::computed_longhand_table::non_inherited_inheritance_changes(
                    old.inheritance_dependent_values(),
                    new.inheritance_dependent_values(),
                ) != 0 =>
            {
                Self::ResetReaders
            }
            impact => impact,
        };
        let stores_moved = !same_arc(new.custom_properties(), old.custom_properties())
            || !same_arc(new.inheritable_custom_properties(), old.inheritable_custom_properties());
        match (impact, stores_moved) {
            (ChildImpact::None, true) => ChildImpact::CustomProperties,
            (ChildImpact::ResetReaders, true) => ChildImpact::ResetReadersAndCustomProperties,
            (impact, _) => impact,
        }
    }

    /// Which children of an element moving from the groups `old` to `new` restyle: all of them
    /// where an inherited group moved, and the ones reading their parent's values of properties
    /// that do not inherit where another did.
    pub(super) fn of_groups(
        new: &crate::css::computed_values::StyleGroups,
        old: &crate::css::computed_values::StyleGroups,
    ) -> Self {
        let group_differs = |group: usize| {
            let (new, old) = (new[group].as_ptr(), old[group].as_ptr());
            // SAFETY: Both are live payloads of the group.
            new != old && !unsafe { crate::css::computed_values::style_group_payloads_equal(group, new, old) }
        };
        let inherited = crate::css::style::computed::ENGINE_INHERITED_GROUP_COUNT;
        // NB: The inherited bits and zoom beside the inherited groups reach the children as the groups do.
        if !new.shares_inherited_groups_with(old)
            && (new.inherited_values() != old.inherited_values() || (0..inherited).any(group_differs))
        {
            return Self::All;
        }
        // NB: The reset bits are read by the children reading their parent's reset values, as
        //     the box group's were.
        if new.reset_bits() != old.reset_bits() {
            return Self::ResetReaders;
        }
        match !new.shares_reset_groups_with(old) && (inherited..group_index::COUNT).any(group_differs) {
            true => Self::ResetReaders,
            false => Self::None,
        }
    }
}

impl RetainedState {
    /// Whether `node`, holding `record`, reads its parent's values of properties that do not
    /// inherit: its record does, or the keyframes of its animations say `inherit` for one.
    pub(super) fn reads_parent_reset_values(&self, node: StyleNodeID, record: &Record) -> bool {
        record.rare().reads.parent_reset_groups != 0 || self.keyframes_reading_parent_reset_values.contains(&node)
    }
}

impl Record {
    /// What the element's computation recorded and read beyond its values, which most elements
    /// share with their siblings, and which the record's rare set holds beside its groups.
    pub(in super::super) fn rare(&self) -> &Arc<RareStyleFacts> {
        self.payloads.rare_facts().expect("a record's rare set holds its facts")
    }

    /// The custom declarations the element's cascade decided, as written, which a sample of its
    /// animations resolves the ones reading an animated custom property from, and which its rare
    /// set holds beside its groups.
    pub(in super::super) fn custom_declarations(&self) -> Option<&CustomDeclarations> {
        self.payloads.custom_declarations()
    }

    /// The custom properties the element holds, which its inherited set holds beside its groups.
    pub(crate) fn custom_properties(&self) -> Option<&Arc<crate::css::custom_properties::CustomPropertyStore>> {
        self.payloads.custom_properties()
    }

    /// The custom properties the element's children inherit.
    pub(in super::super) fn inheritable_custom_properties(
        &self,
    ) -> Option<&Arc<crate::css::custom_properties::CustomPropertyStore>> {
        match &self.rare().inheritable_custom_properties {
            InheritableCustomProperties::Held => self.custom_properties(),
            InheritableCustomProperties::Other(inheritable) => inheritable.as_ref(),
        }
    }

    /// The identity of the custom-property environment the record was resolved in: that of the store it holds, or
    /// zero where it holds none.
    pub(in super::super) fn custom_property_environment(&self) -> u64 {
        self.custom_properties()
            .map_or(0, super::super::custom_property_environments::environment_identity)
    }

    /// The record holding what this one does, resolved in the custom-property environment of `store` instead.
    pub(in super::super) fn in_custom_property_environment(
        &self,
        store: Option<Arc<crate::css::custom_properties::CustomPropertyStore>>,
    ) -> Self {
        let derived = Self::derived(self, &self.payloads);
        Self {
            payloads: derived.payloads.with_custom_properties(store.as_ref()),
            ..derived
        }
    }

    /// The groups holding the record's computed values, where groups hold them.
    pub(in super::super) fn value_groups(&self) -> &crate::css::computed_values::StyleGroups {
        match &self.rare().longhand_home {
            LonghandHome::Groups => &self.payloads,
            LonghandHome::SourceGroups(source) => &source.groups,
        }
    }

    /// What the element's animations sampled over the style this record samples them over, where
    /// it is a sample of them.
    pub(in super::super) fn sample_of_animations(&self) -> Option<&Sample> {
        match &self.rare().longhand_home {
            LonghandHome::Groups => None,
            LonghandHome::SourceGroups(source) => source.sample.as_deref(),
        }
    }

    /// The computed value of `longhand`.
    pub(in super::super) fn longhand(&self, longhand: LonghandID) -> &RetainedStyleValueData {
        let groups = self.value_groups();
        let property =
            GroupValueLayout::get().resolved(longhand.property(), |property| self.longhand(LonghandID::new(property)));
        let place = GroupValueLayout::get().place(LonghandID::new(property));
        if let Some(field) = place.bit_field() {
            return groups.style_bits().value(field);
        }
        crate::css::computed_values::value_at(
            groups.group(crate::css::computed_values::StyleGroupIndex::new(place.group())),
            place,
        )
    }

    /// The value a group build reads for `property`: its recorded currentcolor-dependent specified
    /// value where a drive did not evaluate it, else its computed value.
    pub(in super::super) fn effective_value(&self, property: u16) -> &RetainedStyleValueData {
        self.rare()
            .value_facts
            .value_read_in_place(property)
            .unwrap_or_else(|| self.longhand(LonghandID::new(property)))
    }

    /// The recorded specified values of the longhands whose computed values depend on what the
    /// element inherits.
    pub(in super::super) fn inheritance_dependent_values(
        &self,
    ) -> impl Iterator<Item = (u16, &RetainedStyleValueData)> + Clone {
        self.rare().value_facts.inheritance_dependent_values()
    }

    /// The color scheme the element's colors resolved against.
    pub(in super::super) fn effective_color_scheme(&self) -> i16 {
        self.rare().value_facts.effective_color_scheme()
    }

    /// The groups the children of an element holding the record start their values from.
    pub(in super::super) fn group_seed(&self) -> GroupSeed<'_> {
        GroupSeed::new(self.value_groups(), &self.rare().value_facts)
    }

    /// Whether the element holding the record generates boxes, or its descendants do: whether
    /// they have styles.
    pub(super) fn is_displayed(&self) -> bool {
        !self.facts().display().is_none()
    }

    /// What the record's readers read of the element's style without its groups.
    pub(in super::super) fn facts(&self) -> RecordFacts {
        self.header.facts()
    }

    /// The identity of the slot holding the record, while one does.
    pub(in super::super) fn installed_record(&self) -> Option<InstalledRecordID> {
        self.header.installed_record()
    }

    /// The pseudo-elements of the element that generate a box.
    pub(in super::super) fn pseudo_element_styles(&self) -> PseudoElementStyles {
        self.rare().pseudo_element_styles
    }

    /// This record, styling the pseudo-elements of `styles`: itself where it does, otherwise a copy
    /// sharing all it holds.
    pub(in super::super) fn with_pseudo_element_styles(self: &Arc<Self>, styles: PseudoElementStyles) -> Arc<Self> {
        if self.pseudo_element_styles() == styles {
            return self.clone();
        }
        Arc::new(Self {
            payloads: self
                .payloads
                .clone()
                .with_rare_facts(
                    self.rare().with_pseudo_element_styles(styles),
                    self.custom_declarations().cloned(),
                    &[],
                )
                .with_custom_properties(self.custom_properties()),
            header: RecordHeader::new(self.facts()),
        })
    }

    /// This record, whose computation depends on `dependency_flags` too, as a test makes one.
    #[cfg(test)]
    pub(in super::super) fn with_test_dependency_flags(self: &Arc<Self>, dependency_flags: u8) -> Arc<Self> {
        let mut facts = self.facts();
        facts.add_dependency_flags(dependency_flags);
        Arc::new(Self {
            payloads: self.payloads.clone(),
            header: RecordHeader::new(facts),
        })
    }

    /// Name the identity of the slot that takes the record, and what a view of it depends on, and
    /// answer whether it did: a record a fork's slot names already keeps that name.
    #[must_use]
    pub(in super::super) fn name_installed_record(&self, id: InstalledRecordID, view_dependency_flags: u8) -> bool {
        self.settle_view_dependency_flags(view_dependency_flags);
        self.header.name_installed_record(id)
    }

    /// A copy of the record sharing all it holds, which no slot names.
    pub(in super::super) fn copy_without_a_name(&self) -> Self {
        Self {
            payloads: self.payloads.clone(),
            header: RecordHeader::new(self.facts()),
        }
    }

    /// Settle what a view of the record depends on for a holder no slot names it to.
    pub(in super::super) fn settle_view_dependency_flags(&self, view_dependency_flags: u8) {
        self.header
            .set_holds_image_values(view_dependency_flags & computed::HOLDS_IMAGE_VALUES != 0);
    }

    /// What a view of the record depends on, as the slot holding it last settled it.
    pub(in super::super) fn view_dependency_flags(&self) -> u8 {
        computed::view_dependency_flags(&self.facts(), self.header.holds_image_values())
    }

    /// Settles whether the groups of the record hold images: on the thread that built them, which has them at hand, and
    /// among others, for the serial step that names the record's slot to read a flag.
    pub(in super::super) fn settle_holds_image_values(&self) {
        // SAFETY: The record holds a reference to each of its groups.
        let holds_image_values =
            unsafe { crate::css::computed_values::style_group_payloads_hold_image_values(&self.payloads.pointers()) };
        self.header.set_holds_image_values(holds_image_values);
    }

    /// What a view of the record depends on, for a slot to take it.
    pub(in super::super) fn dependency_flags_for_a_slot(&self) -> u8 {
        match self.header.settled_holds_image_values() {
            Some(holds_image_values) => computed::view_dependency_flags(&self.facts(), holds_image_values),
            None => computed::record_dependency_flags(&self.facts(), &self.payloads),
        }
    }

    /// Forget the identity of the slot that let go of the record.
    pub(in super::super) fn forget_installed_record(&self) {
        self.header.forget_installed_record();
    }

    /// Whether `other` holds what this record does, the pseudo-elements it styles included.
    pub(in super::super) fn holds_the_same_as(&self, other: &Record) -> bool {
        self.pseudo_element_styles() == other.pseudo_element_styles() && self.holds_the_same_style_as(other)
    }

    /// Whether `other` holds the style this record does, whatever pseudo-elements either styles,
    /// as a pass compares a record it computed with one it held before it settles them.
    pub(in super::super) fn holds_the_same_style_as(&self, other: &Record) -> bool {
        self.holds_the_same_besides_its_table_as(other)
            && self.rare().value_facts.publication_equals(&other.rare().value_facts)
            && self.holds_the_same_values_as(other)
            && self.rare().table_flags.publication_equals(&other.rare().table_flags)
    }

    /// Whether `other` holds the computed values this record does.
    fn holds_the_same_values_as(&self, other: &Record) -> bool {
        let (mine, theirs) = (self.value_groups(), other.value_groups());
        // NB: The longhands a derived record answers with are its source's, whose bits can differ from its own: a
        //     table's box holds none of the outer properties the table computes, and an anonymous box's source can
        //     hold another writing mode.
        mine.style_bits() == theirs.style_bits()
            && mine
                .iter()
                .zip(theirs.iter())
                .enumerate()
                .all(|(group, (mine, theirs))| {
                    mine.as_ptr() == theirs.as_ptr()
                // SAFETY: Both records hold a reference to each of their value groups.
                || unsafe { crate::css::computed_values::hold_the_same_values(group, mine.as_ptr(), theirs.as_ptr()) }
                })
    }

    /// Whether `other` holds the custom declarations this record does: the element's own, which
    /// moving custom properties builds its custom properties from again. A name has one winner, so
    /// the order they stand in says nothing, and what a declaration holds counts by value, as a
    /// cascade makes the value of `initial` anew.
    fn holds_the_same_custom_declarations_as(&self, other: &Record) -> bool {
        match (&self.custom_declarations(), &other.custom_declarations()) {
            (None, None) => true,
            (Some(mine), Some(theirs)) => {
                let same = |mine: &(CustomDeclaration, crate::css::style_value::RetainedStyleValueData),
                            theirs: &(CustomDeclaration, crate::css::style_value::RetainedStyleValueData)| {
                    mine.0 == theirs.0 && (mine.1.pointer() == theirs.1.pointer() || mine.1 == theirs.1)
                };
                Arc::ptr_eq(mine, theirs)
                    || (mine.len() == theirs.len()
                        && mine.iter().enumerate().all(|(index, declaration)| {
                            same(declaration, &theirs[index])
                                || theirs
                                    .iter()
                                    .any(|their_declaration| same(declaration, their_declaration))
                        }))
            }
            _ => false,
        }
    }

    /// Whether `other` holds what this record does, its longhand table aside.
    fn holds_the_same_besides_its_table_as(&self, other: &Record) -> bool {
        let same_store = crate::css::custom_properties::CustomPropertyStore::absent_or_holding_the_same_values;
        self.payloads.current_color() == other.payloads.current_color()
            && self.rare().custom_property_reads == other.rare().custom_property_reads
            && self.rare().reads == other.rare().reads
            && self.rare().counter_style_environment_identity == other.rare().counter_style_environment_identity
            && self.holds_the_same_custom_declarations_as(other)
            && self.rare().animation_name_scope == other.rare().animation_name_scope
            && same_store(self.custom_properties(), other.custom_properties())
            && same_store(
                self.inheritable_custom_properties(),
                other.inheritable_custom_properties(),
            )
            && self.payloads.holds_equal_payloads_as(&other.payloads)
            && self.facts() == other.facts()
    }
}

impl Record {
    /// The record of a style the host computed into `values`, with the flags `flags` and the
    /// dependency flags `dependency_flags` its computation settled beside them, which holds the
    /// default groups until the host builds its own. No element computes against it.
    pub(in super::super) fn from_values(
        values: crate::css::computed_values::GroupValues<'_>,
        flags: crate::css::computed_longhand_table::LonghandTableFlags,
        dependency_flags: u8,
        defaults: &crate::css::computed_values::DefaultGroups,
    ) -> Self {
        let facts = RecordFacts::of_values(|property| values.value(property), dependency_flags, false);
        let value_facts = values.facts().clone();
        let style_bits = values.style_bits();
        // NB: The groups hold the values in their value arrays and in their payload handles.
        let value_groups = values.into_groups();
        Self {
            payloads: defaults
                .groups()
                .clone()
                .with_style_bits(style_bits)
                .with_rare_facts(
                    Arc::new(RareStyleFacts::of_host_style(flags, value_facts, value_groups, 0)),
                    None,
                    &[],
                )
                .with_custom_properties(None),
            header: RecordHeader::new(facts),
        }
    }

    /// The record of a style holding `value_of(property)` for each longhand, as a test makes one.
    #[cfg(test)]
    pub(crate) fn of_test_values(value_of: impl Fn(u16) -> RetainedStyleValueData) -> Self {
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
        let defaults = crate::css::computed_values::DefaultGroups::default();
        crate::css::computed_values::with_default_seed(&defaults, |seed| {
            let mut writer = crate::css::computed_values::GroupValuesWriter::seeded_for_child(
                seed,
                seed,
                std::iter::empty(),
                crate::css::computed_longhand_table::LonghandTableFlags::empty(),
                0,
            );
            for property in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
                let value = value_of(property);
                if GroupValueLayout::get().holds_value(LonghandID::new(property)) && !value.pointer().is_null() {
                    writer.set(property, value);
                }
            }
            let (values, flags, dependency_flags) = writer.finish(&[]);
            Self::from_values(values, flags, dependency_flags, &defaults)
        })
    }

    /// The record of a style the host made, holding `payloads`, whose computed values are the
    /// initial ones of `initial`, which it keeps the groups of. No element computes against it.
    pub(in super::super) fn of_initial(
        initial: &InitialStyle,
        payloads: &crate::css::computed_values::StyleGroups,
        counter_style_environment_identity: u64,
    ) -> Self {
        Self {
            payloads: payloads
                .clone()
                .with_rare_facts(
                    Arc::new(RareStyleFacts::of_host_style(
                        initial.flags.clone(),
                        initial.value_facts.clone(),
                        initial.payloads.clone(),
                        counter_style_environment_identity,
                    )),
                    None,
                    &[],
                )
                .with_custom_properties(None),
            header: RecordHeader::new(RecordFacts::of_values(
                |property| initial.longhand(LonghandID::new(property)),
                initial.dependency_flags,
                false,
            )),
        }
    }

    /// Whether the record's values derive from the metrics of the first available font of the
    /// element or its parent, which a load filling one of its faces in moves: a length relative to
    /// one of them, or a `font-size-adjust`, which sizes the font by them. A `line-height: normal`
    /// does not: its used value is read through the cascade.
    pub(super) fn reads_font_metrics(&self) -> bool {
        use crate::css::property_metadata::property_id;
        use crate::css::style_compute::keyword;
        let is_keyword = |property, expected| {
            matches!(
                self.longhand(LonghandID::new(property)).optional_data(),
                Some(crate::css::style_value::StyleValueData::Keyword { keyword }) if *keyword == expected
            )
        };
        self.rare().reads.reads_font_metric_lengths || !is_keyword(property_id::FONT_SIZE_ADJUST, keyword::NONE)
    }

    /// The record of a style derived from that of `source` holding `payloads` in place of its
    /// groups, such as an anonymous box's or a sample of animations over it, which holds the
    /// longhands of `source`.
    pub(in super::super) fn derived(source: &Record, payloads: &crate::css::computed_values::StyleGroups) -> Self {
        Self::derived_with(source, payloads, |_| {})
    }

    /// The record `derived` answers, with its rare facts as `adjust` leaves them.
    fn derived_with(
        source: &Record,
        payloads: &crate::css::computed_values::StyleGroups,
        adjust: impl FnOnce(&mut RareStyleFacts),
    ) -> Self {
        let mut rare = RareStyleFacts::clone(source.rare());
        if matches!(rare.longhand_home, LonghandHome::Groups) {
            rare.longhand_home = LonghandHome::SourceGroups(DerivedSource::of(source.payloads.clone()));
        }
        adjust(&mut rare);
        Self {
            payloads: payloads
                .clone()
                .with_current_color(source.payloads.current_color())
                .with_rare_facts(Arc::new(rare), source.custom_declarations().cloned(), &[])
                .with_custom_properties(source.custom_properties()),
            header: RecordHeader::new(source.facts()),
        }
    }
}

impl Record {
    /// The record of a sample of the animations of an element over the installed record `base`,
    /// holding `payloads`, the groups of `base` with `animated_overlay` composed into them, whose
    /// views depend on `dependency_flags`, named `identity`.
    pub(in super::super) fn sample(
        base: &Arc<Record>,
        payloads: &crate::css::computed_values::StyleGroups,
        animated_overlay: Arc<crate::css::animated_overlay::AnimatedOverlay>,
        dependency_flags: u8,
        identity: computed::FinalStyleRecordID,
    ) -> Self {
        Self::derived_with(base, payloads, |rare| {
            let LonghandHome::SourceGroups(source) = &mut rare.longhand_home else {
                unreachable!("a derived record holds the longhands of its source");
            };
            source.sample = Some(Box::new(Sample {
                base: base.clone(),
                animated_overlay,
                dependency_flags,
                identity,
            }));
        })
    }
}

impl RetainedState {
    /// Which children of `node`, an element computing `new` where it computed `old`, compute anew:
    /// those computing from something `new` differs in from what they inherited before, `old` or
    /// the composition of its animations over that. A child computes from its parent's inherited
    /// groups, color, zoom, publication dependencies, display and custom properties, as a result
    /// is reused by, and from its other groups only where it read them.
    pub(super) fn child_impact_of_records(
        &self,
        node: StyleNodeID,
        new: &Arc<Record>,
        old: &Arc<Record>,
    ) -> ChildImpact {
        if Arc::ptr_eq(new, old) {
            return ChildImpact::None;
        }
        // The children inherited the composition of the element's animations over the record it
        // held, where the host composed one, and the custom properties those sampled.
        let composition = self.computed_group_sets.composition_over_record(node, old);
        if self.sampled_custom_property_store(node).is_some() {
            return ChildImpact::All;
        }
        ChildImpact::of_records(new, old, composition.map_or(&old.payloads, |view| view.payloads))
    }

    /// The custom properties the animations of `node` sampled, where the host sampled them over
    /// the environment of the record the element holds, borrowed.
    fn sampled_custom_property_store(
        &self,
        node: StyleNodeID,
    ) -> Option<Option<&Arc<crate::css::custom_properties::CustomPropertyStore>>> {
        let environment = self.computed_group_sets.custom_property_environment_identity(node);
        let sampled = self
            .element_custom_property_data
            .get(&node)
            .filter(|held| {
                held.sampled_over.is_some() && (environment == Some(held.identity) || held.sampled_over == environment)
            })?
            .identity;
        Some(self.custom_property_environments.store(sampled))
    }

    /// Install the record of a style the host made holding `payloads`, whose computed values are
    /// the initial ones, and answer its identity. Nothing holds
    /// the record yet: the next sweep frees it unless a holder pins it first.
    pub(in super::super) fn install_initial_derived_record(
        &mut self,
        payloads: &crate::css::computed_values::StyleGroups,
        counter_style_environment_identity: u64,
    ) -> computed::FinalStyleRecordID {
        let initial = self
            .live_style
            .as_ref()
            .map(|live| live.initial.clone())
            .expect("a style the host made inherits from one a pass styled");
        let record = Record::of_initial(&initial, payloads, counter_style_environment_identity);
        self.install_new_record(Arc::new(record), 0)
    }

    /// Install the record of a style derived from that of the record `source` is or composes
    /// over, holding `payloads`, and answer its identity, or none
    /// where `source` is not a record the engine installed. Nothing holds the record yet: the next
    /// sweep frees it unless a holder pins it first.
    pub(in super::super) fn install_record_derived_from(
        &mut self,
        source: computed::FinalStyleRecordID,
        payloads: &crate::css::computed_values::StyleGroups,
    ) -> Option<computed::FinalStyleRecordID> {
        let source = self.computed_group_sets.installed_base_record(source)?.clone();
        Some(self.install_record_derived(&source, payloads, None))
    }

    /// Install the record of a style derived from `source`, holding `payloads`, in the counter style
    /// registry `counter_style_environment_identity` names where it names one other than the
    /// source's, and answer its identity. Nothing holds the
    /// record yet: the next sweep frees it unless a holder pins it first.
    pub(in super::super) fn install_record_derived(
        &mut self,
        source: &Record,
        payloads: &crate::css::computed_values::StyleGroups,
        counter_style_environment_identity: Option<u64>,
    ) -> computed::FinalStyleRecordID {
        let record = Arc::new(Record::derived_with(source, payloads, |rare| {
            if let Some(identity) = counter_style_environment_identity {
                rare.counter_style_environment_identity = identity;
            }
        }));
        self.install_new_record(record, 0)
    }

    /// Install the record of a sample composed over `source`, holding `payloads`, found by `overlay_hash`, the hash of
    /// what the sample animates, and answer its identity. Nothing holds the record yet: the next sweep frees it unless
    /// a holder pins it first.
    pub(in super::super) fn install_sample_record(
        &mut self,
        source: &Record,
        payloads: &crate::css::computed_values::StyleGroups,
        overlay_hash: u64,
    ) -> computed::FinalStyleRecordID {
        self.install_new_record(Arc::new(Record::derived(source, payloads)), overlay_hash)
    }

    fn install_new_record(&mut self, record: Arc<Record>, key_salt: u64) -> computed::FinalStyleRecordID {
        self.computed_group_sets.install_derived_record(&record, key_salt, || {
            computed::record_dependency_flags(&record.facts(), &record.payloads)
        })
    }
}

impl Record {
    /// Counts what `record` holds, by owner, the first time the report reaches it: the record
    /// itself, its groups with their values, and its custom properties.
    pub(in super::super) fn report_memory(record: &Arc<Record>, report: &mut MemoryReport) {
        use crate::heap_size::HeapSize;
        if !report.sizer.first_sight(Arc::as_ptr(record)) {
            return;
        }
        report.add(
            MemoryOwner::Records,
            crate::heap_size::ARC_HEADER_BYTES + size_of::<Record>(),
        );
        // NB: The rare facts count once, by the first record reaching them.
        if report.sizer.first_sight(Arc::as_ptr(record.rare())) {
            let rare = record.rare();
            let custom_property_reads = match &rare.custom_property_reads {
                crate::css::style::custom_property_cascade::LonghandCustomPropertyReads::Named(names) => {
                    names.heap_size(&mut report.sizer)
                }
                _ => 0,
            };
            report.add(
                MemoryOwner::Records,
                crate::heap_size::ARC_HEADER_BYTES + size_of::<RareStyleFacts>() + custom_property_reads,
            );
            report.count(MemoryOwner::GroupValueFacts, &rare.value_facts);
        }
        // NB: The custom declarations count once, by the first record reaching them.
        let custom_declarations = record
            .custom_declarations()
            .map_or(0, |declarations| declarations.heap_size(&mut report.sizer));
        report.add(MemoryOwner::Records, custom_declarations);
        // NB: The group sets count once, by the first record reaching them.
        for (set, bytes) in record.payloads.set_allocations() {
            if report.sizer.first_sight(set) {
                report.add(MemoryOwner::Records, bytes);
            }
        }
        // NB: A group's allocation holds its computed values after its payload, and the style
        //     values those hold count once, the first time any owner reaches them.
        let mut payloads = 0;
        for (group, payload) in record.payloads.iter().enumerate() {
            if !report.sizer.first_sight(payload.as_ptr()) {
                continue;
            }
            payloads += crate::css::computed_values::group_payload_allocation_bytes(group, payload.as_ptr());
            // SAFETY: The record holds a reference to each of its groups.
            payloads += unsafe {
                crate::css::computed_values::group_held_values_heap_size(group, payload.as_ptr(), &mut report.sizer)
            };
        }
        report.add(MemoryOwner::Payloads, payloads);
        if let Some(store) = record.custom_properties() {
            report.count(MemoryOwner::CustomProperties, store);
        }
        if let InheritableCustomProperties::Other(inheritable) = &record.rare().inheritable_custom_properties {
            report.count(MemoryOwner::CustomProperties, inheritable);
        }
    }
}

impl crate::heap_size::HeapSize for CustomDeclaration {
    fn heap_size(&self, _: &mut crate::heap_size::HeapSizer) -> usize {
        0
    }
}

impl crate::heap_size::HeapSize for ContainerVerdict {
    fn heap_size(&self, _: &mut crate::heap_size::HeapSizer) -> usize {
        crate::heap_size::vec_bytes(&self.effects)
    }
}

impl Held {
    fn report_memory(&self, report: &mut MemoryReport) {
        Record::report_memory(&self.record, report);
        report.count(MemoryOwner::RetainedSlots, &self.matches);
    }
}

fn report_pseudo_element_records_memory(records: &PseudoElementRecords, report: &mut MemoryReport) {
    report.add(MemoryOwner::RetainedSlots, crate::heap_size::small_vec_bytes(records));
    for (_, record) in records {
        Record::report_memory(record, report);
    }
}

/// The custom properties an element holds or hands down, where it holds any.
pub(super) type HeldCustomProperties = Option<Arc<crate::css::custom_properties::CustomPropertyStore>>;

/// The custom properties an element holds or hands down, as a pass reads them: borrowed from its
/// record, unless its animations sampled them.
pub(super) type BorrowedCustomProperties<'a> = Option<Cow<'a, Arc<crate::css::custom_properties::CustomPropertyStore>>>;

pub(super) type PseudoElementRecords = smallvec::SmallVec<[(PseudoElementTarget, Arc<Record>); 2]>;

/// https://drafts.csswg.org/css-pseudo-4/#highlight-cascade
/// Whether the highlight pseudo-elements of `new` hold other styles than those of `old`, which the
/// highlight pseudo-elements of an element's children inherit from.
pub(super) fn highlights_moved(
    new: &[(PseudoElementTarget, Arc<Record>)],
    old: &[(PseudoElementTarget, Arc<Record>)],
) -> bool {
    let of_target = |records: &'_ [(PseudoElementTarget, Arc<Record>)], target: PseudoElementTarget| {
        records
            .iter()
            .find(|(held, _)| *held == target)
            .map(|(_, record)| record.clone())
    };
    new.iter()
        .chain(old)
        .map(|&(target, _)| target)
        .filter(|target| super::super::publication::pseudo_kind::is_highlight(target.kind))
        .any(|target| match (of_target(new, target), of_target(old, target)) {
            (None, None) => false,
            (Some(new), Some(old)) => !Arc::ptr_eq(&new, &old) && !new.holds_the_same_as(&old),
            _ => true,
        })
}

/// One element's record, with the identity it shares under. Elements holding one record need not
/// share an identity: a record reused for an equal cascade says nothing of the elements' selectors.
/// The rule index entries an element matched, in index order, behind one thin pointer, which the
/// elements that matched alike share.
pub(in super::super) type MatchList = Arc<Box<[u32]>>;

#[derive(Clone)]
pub(super) struct Held {
    record: Arc<Record>,
    sharing_identity: StyleNodeID,
    /// The rule index entries the element matched, in index order, where the pass kept them.
    matches: Option<MatchList>,
}

/// What the passes before kept of one element that few elements have.
#[derive(Clone, Default)]
struct RareRetained {
    pseudo_element_records: PseudoElementRecords,
    /// What the container conditions of the element's matches read of its containers.
    container_reads: Option<Arc<ContainerVerdict>>,
}

/// What the passes before kept of one element.
#[derive(Clone, Default)]
struct RetainedSlot {
    held: Option<Held>,
    /// The element's pseudo-element records and container reads, where it has either.
    rare: Option<Box<RareRetained>>,
}

impl RetainedSlot {
    fn is_empty(&self) -> bool {
        self.held.is_none() && self.rare.is_none()
    }

    fn pseudo_element_records(&self) -> &[(PseudoElementTarget, Arc<Record>)] {
        self.rare
            .as_ref()
            .map_or(&[], |rare| rare.pseudo_element_records.as_slice())
    }

    fn container_reads(&self) -> Option<&Arc<ContainerVerdict>> {
        self.rare.as_ref()?.container_reads.as_ref()
    }
}

// NB: Every element a pass styled has one of these.
const _: () = assert!(size_of::<RetainedSlot>() == 32);

/// How many element slots a page of retained records holds.
const RETAINED_PAGE_SIZE: usize = 256;

/// How many element slots the first page of retained records holds at least: a document of a few
/// elements holds few slots, and the first page doubles as its document grows.
const RETAINED_FIRST_PAGE_MIN_SIZE: usize = 8;

fn empty_retained_page(length: usize) -> Arc<[RetainedSlot]> {
    (0..length).map(|_| RetainedSlot::default()).collect()
}

/// The records the passes before kept, by element, which a pass that restyles some elements reads
/// for the others.
///
/// A fork of the render state takes the records in pages it shares with the state it forked until
/// either side writes one: forking costs one reference count, and the first write to a page after
/// a fork copies the page alone.
#[derive(Clone, Default)]
pub(super) struct RetainedRecords {
    /// Each page holds `RETAINED_PAGE_SIZE` slots but for the first, until the page after it exists.
    pages: crate::cow_column::CowVec<Arc<[RetainedSlot]>, 64>,
    /// The root font inputs the last pass that styled the document element resolved
    /// root-relative lengths against, with that element. A record reads the metrics of its first
    /// available font through its cascade, which a load filling one of its faces in moves, so what
    /// the descendants resolved against is kept as the pass found it rather than read again from
    /// the record.
    root_font: Option<(StyleNodeID, super::compute::RootFontInputs)>,
}

impl RetainedRecords {
    /// Counts the pages and what their slots hold, by owner.
    pub(super) fn report_memory(&self, report: &mut MemoryReport) {
        report.add(MemoryOwner::RetainedSlots, self.pages.row_bytes());
        for page in self.pages.iter() {
            if !report.sizer.first_sight(page.as_ptr()) {
                continue;
            }
            report.add(
                MemoryOwner::RetainedSlots,
                crate::heap_size::ARC_HEADER_BYTES + size_of_val::<[RetainedSlot]>(page),
            );
            for slot in page.iter() {
                if let Some(held) = &slot.held {
                    report.styled_elements += 1;
                    held.report_memory(report);
                }
                if let Some(rare) = &slot.rare {
                    report.add(MemoryOwner::RetainedSlots, size_of::<RareRetained>());
                    report_pseudo_element_records_memory(&rare.pseudo_element_records, report);
                    report.count(MemoryOwner::RetainedSlots, &rare.container_reads);
                }
            }
        }
    }

    /// Keep the root font inputs a pass that styled the document element `node` resolved against.
    pub(super) fn keep_root_font(&mut self, node: StyleNodeID, root_font: super::compute::RootFontInputs) {
        self.root_font = Some((node, root_font));
    }

    /// The root font inputs the last pass that styled the document element `node` resolved against, where one did.
    pub(super) fn kept_root_font(&self, node: StyleNodeID) -> Option<super::compute::RootFontInputs> {
        let (kept, root_font) = self.root_font?;
        (kept == node).then_some(root_font)
    }

    /// Whether the style of `node`, its own or one of its pseudo-elements', derives from the
    /// metrics of a first available font, or is not known not to, having no record.
    pub(super) fn reads_font_metrics(&self, node: StyleNodeID) -> bool {
        self.get(node).is_none_or(|record| record.reads_font_metrics())
            || self
                .pseudo_element_records(node)
                .iter()
                .any(|(_, record)| record.reads_font_metrics())
    }

    fn slot(&self, slot: usize) -> Option<&RetainedSlot> {
        self.pages
            .get(slot / RETAINED_PAGE_SIZE)?
            .get(slot % RETAINED_PAGE_SIZE)
    }

    /// The slot, for writing, which copies its page where a fork shares it.
    fn slot_mut(&mut self, slot: usize) -> Option<&mut RetainedSlot> {
        let page = self.pages.get_mut(slot / RETAINED_PAGE_SIZE)?;
        Arc::make_mut(page).get_mut(slot % RETAINED_PAGE_SIZE)
    }

    /// Make room for `slot_count` elements.
    fn reserve(&mut self, slot_count: usize) {
        let first_page_length = slot_count
            .next_power_of_two()
            .clamp(RETAINED_FIRST_PAGE_MIN_SIZE, RETAINED_PAGE_SIZE);
        match self.pages.get(0) {
            Some(first) if first.len() < first_page_length => {
                let mut slots = Vec::with_capacity(first_page_length);
                slots.extend_from_slice(first);
                slots.resize_with(first_page_length, RetainedSlot::default);
                self.pages[0] = slots.into();
            }
            None if slot_count != 0 => self.pages.push(empty_retained_page(first_page_length)),
            _ => {}
        }
        let page_count = slot_count.div_ceil(RETAINED_PAGE_SIZE);
        while self.pages.len() < page_count {
            self.pages.push(empty_retained_page(RETAINED_PAGE_SIZE));
        }
    }

    fn held(&self, node: StyleNodeID) -> Option<&Held> {
        self.slot(node.element_slot())?.held.as_ref()
    }

    pub(super) fn get(&self, node: StyleNodeID) -> Option<&Arc<Record>> {
        Some(&self.held(node)?.record)
    }

    /// The rule index entries `node` matched, where the passes kept them.
    pub(super) fn matches_of(&self, node: StyleNodeID) -> Option<&MatchList> {
        self.held(node)?.matches.as_ref()
    }

    pub(super) fn pseudo_element_records(&self, node: StyleNodeID) -> &[(PseudoElementTarget, Arc<Record>)] {
        self.slot(node.element_slot())
            .map_or(&[], RetainedSlot::pseudo_element_records)
    }

    /// What the container conditions of the rules `node` matched, its pseudo-elements' included,
    /// read of its containers, where it matched any rule applying under container conditions.
    pub(super) fn container_reads(&self, node: StyleNodeID) -> Option<&Arc<ContainerVerdict>> {
        self.slot(node.element_slot())?.container_reads()
    }

    /// Keep the records a pass computed for the elements of `styled`, over the ones they replace,
    /// emptying their cells for the next pass, and note in `changed` the elements whose records,
    /// or pseudo-element records, are not the ones they held. The records replaced go to
    /// `replaced`, to drop wherever is convenient.
    pub(super) fn absorb(
        &mut self,
        cells: &mut RecordCells,
        styled: &[StyleNodeID],
        mut changed: Option<&mut Vec<StyleNodeID>>,
        replaced: &mut Replaced,
    ) {
        self.reserve(cells.slot_count);
        // The copies of the records the pass computed that style the pseudo-elements their
        // elements do, one per record and set of pseudo-elements, which elements sharing a style
        // share.
        let mut with_pseudo_element_styles: HashMap<(usize, PseudoElementStyles), Arc<Record>> = HashMap::default();
        for (index, node) in styled.iter().enumerate() {
            if let Some(ahead) = styled.get(index + RECORDS_AHEAD) {
                cells.prefetch_record(*ahead);
            }
            let slot = node.element_slot();
            let Some(page) = cells.pages[slot / CELL_PAGE_SIZE].get_mut() else {
                continue;
            };
            let cell = slot % CELL_PAGE_SIZE;
            if let Some(mut held) = page.held[cell].take() {
                let container_reads = page.container_reads[cell].take();
                let reads_containers = container_reads.is_some();
                let mut pseudo_element_records = page.pseudo_element_records[cell].take().unwrap_or_default();
                let styles = PseudoElementStyles::of(&pseudo_element_records);
                if held.record.pseudo_element_styles() != styles {
                    held.record = with_pseudo_element_styles
                        .entry((Arc::as_ptr(&held.record) as usize, styles))
                        .or_insert_with(|| held.record.with_pseudo_element_styles(styles))
                        .clone();
                }
                let retained = self
                    .slot_mut(slot)
                    .expect("the records have room for every styled element");
                // NB: Where the records are installed, one holding what the record it replaces
                //     does gives way to it, which the host holds already.
                if let Some(changed) = changed.as_deref_mut() {
                    let old = retained.pseudo_element_records();
                    let mut same_pseudo_element_records = old.len() == pseudo_element_records.len();
                    for (new, old) in pseudo_element_records.iter_mut().zip(old) {
                        if new.0 != old.0 {
                            same_pseudo_element_records = false;
                        } else if !Arc::ptr_eq(&new.1, &old.1) {
                            match new.1.holds_the_same_as(&old.1) {
                                true => new.1 = old.1.clone(),
                                false => same_pseudo_element_records = false,
                            }
                        }
                    }
                    let same_record = match &retained.held {
                        Some(old) if Arc::ptr_eq(&old.record, &held.record) => true,
                        Some(old) if held.record.holds_the_same_as(&old.record) => {
                            held.record = old.record.clone();
                            true
                        }
                        _ => false,
                    };
                    // What an element's container conditions read is the host's to record as it
                    // installs the element's record, which it does again where they read anew.
                    if !same_record || !same_pseudo_element_records || reads_containers {
                        changed.push(*node);
                    }
                }
                if retained
                    .held
                    .as_ref()
                    .is_some_and(|old| !old.record.is_displayed() && held.record.is_displayed())
                {
                    replaced.revealed.push(*node);
                }
                replaced.records.extend(retained.held.replace(held));
                let mut rare = retained.rare.take();
                let replaced_pseudo_element_records = rare
                    .as_mut()
                    .map(|rare| std::mem::take(&mut rare.pseudo_element_records))
                    .unwrap_or_default();
                if !pseudo_element_records.is_empty() || container_reads.is_some() {
                    let mut rare = rare.unwrap_or_default();
                    rare.pseudo_element_records = pseudo_element_records;
                    rare.container_reads = container_reads;
                    retained.rare = Some(rare);
                }
                if !replaced_pseudo_element_records.is_empty() {
                    replaced.pseudo_element_records.push(replaced_pseudo_element_records);
                }
            }
        }
        cells.release();
    }

    /// Renumber the matches the records hold for a rebuilt rule index, where `renumbering` has the
    /// entry standing for each one of the index before. An element that matched an entry the
    /// rebuilt index lost holds no matches, and is matched against the whole index when restyled.
    pub(super) fn renumber_matches(&mut self, renumbering: &[Option<u32>]) {
        // Elements sharing a style hold one list, which they keep sharing. The lists renumbered
        // are kept until the end, so that no other list takes the place of one of them.
        let mut renumbered: HashMap<*const u32, Option<MatchList>> = HashMap::default();
        let mut originals: Vec<MatchList> = Vec::new();
        for page_index in 0..self.pages.len() {
            // NB: A page holding no matches is left to the fork it may be shared with.
            if !self.pages[page_index]
                .iter()
                .any(|slot| slot.held.as_ref().is_some_and(|held| held.matches.is_some()))
            {
                continue;
            }
            for held in Arc::make_mut(&mut self.pages[page_index])
                .iter_mut()
                .filter_map(|slot| slot.held.as_mut())
            {
                let Some(matches) = held.matches.take() else {
                    continue;
                };
                held.matches = renumbered
                    .entry(matches.as_ptr())
                    .or_insert_with(|| {
                        // A list naming no entry that moved is kept as it is.
                        if matches
                            .iter()
                            .all(|&entry| renumbering.get(entry as usize).copied().flatten() == Some(entry))
                        {
                            return Some(Arc::clone(&matches));
                        }
                        matches
                            .iter()
                            .map(|&entry| renumbering.get(entry as usize).copied().flatten())
                            .collect::<Option<Vec<u32>>>()
                            .map(|mut new| {
                                new.sort_unstable();
                                new.dedup();
                                Arc::new(new.into_boxed_slice())
                            })
                    })
                    .clone();
                originals.push(matches);
            }
        }
    }

    /// Have `node` hold `matches` in place of the entries it matched, where it holds a record.
    pub(super) fn replace_matches(&mut self, node: StyleNodeID, matches: MatchList) {
        if let Some(held) = self.slot_mut(node.element_slot()).and_then(|slot| slot.held.as_mut()) {
            held.matches = Some(matches);
        }
    }

    /// Have `node` hold `installed` in place of its record at the address `own`, which holds what
    /// `installed` does, where it still holds that record.
    pub(super) fn adopt(&mut self, node: StyleNodeID, own: usize, installed: Arc<Record>) {
        let slot = node.element_slot();
        if !self
            .slot(slot)
            .and_then(|retained| retained.held.as_ref())
            .is_some_and(|held| Arc::as_ptr(&held.record) as usize == own)
        {
            return;
        }
        if let Some(held) = self.slot_mut(slot).and_then(|retained| retained.held.as_mut()) {
            held.record = installed;
        }
    }

    /// Forget the record of an element that left the tree.
    pub(super) fn forget(&mut self, node: StyleNodeID) {
        let slot = node.element_slot();
        if self.slot(slot).is_some_and(|retained| !retained.is_empty())
            && let Some(retained) = self.slot_mut(slot)
        {
            *retained = RetainedSlot::default();
        }
    }
}

/// The records a pass replaced.
#[derive(Default)]
pub(super) struct Replaced {
    records: Vec<Held>,
    pseudo_element_records: Vec<PseudoElementRecords>,
    /// The elements whose records the pass moved from one that is not displayed to one that is.
    pub(super) revealed: Vec<StyleNodeID>,
}

impl Replaced {
    /// How many records, or sets of pseudo-element records, were replaced.
    pub(super) fn len(&self) -> usize {
        self.records.len() + self.pseudo_element_records.len()
    }

    /// Drop every record replaced, keeping the room they took.
    pub(super) fn clear(&mut self) {
        self.records.clear();
        self.pseudo_element_records.clear();
        self.revealed.clear();
    }
}

/// The cells a pass writes the records it computes to, by element slot.
#[derive(Default)]
pub(super) struct RecordCells {
    /// How many elements the cells have room for.
    slot_count: usize,
    /// The cells of the elements, by page of `CELL_PAGE_SIZE` element slots: only the pages a
    /// pass writes to are made, as it first writes to them, and they go once their records are
    /// kept, so the cells take no room between passes.
    pages: Vec<std::sync::OnceLock<Box<CellPage>>>,
}

/// How many elements a page of record cells holds.
const CELL_PAGE_SIZE: usize = 64;

/// How many elements ahead a thread going through the records a pass made on other threads asks for their memory.
pub(super) const RECORDS_AHEAD: usize = 8;

impl RecordCells {
    /// Asks for the memory of the record the pass made for `node`.
    fn prefetch_record(&self, node: StyleNodeID) {
        let slot = node.element_slot();
        if let Some(page) = self.pages[slot / CELL_PAGE_SIZE].get()
            && let Some(held) = page.held[slot % CELL_PAGE_SIZE].get()
        {
            held.record.prefetch();
        }
    }
}

/// The record cells of `CELL_PAGE_SIZE` elements.
struct CellPage {
    /// Elements that share a style hold one record.
    held: [WriteOnce<Held>; CELL_PAGE_SIZE],
    /// The records of each element's pseudo-elements that generate a box.
    pseudo_element_records: [WriteOnce<PseudoElementRecords>; CELL_PAGE_SIZE],
    /// What the container conditions of each element's matches read of its containers, where it
    /// matched a rule applying under container conditions. An element's own, not its record's:
    /// elements whose cascades decide alike share a record, but not their containers.
    container_reads: [WriteOnce<Arc<ContainerVerdict>>; CELL_PAGE_SIZE],
}

impl Default for CellPage {
    fn default() -> Self {
        Self {
            held: std::array::from_fn(|_| WriteOnce::default()),
            pseudo_element_records: std::array::from_fn(|_| WriteOnce::default()),
            container_reads: std::array::from_fn(|_| WriteOnce::default()),
        }
    }
}

impl RecordCells {
    /// Counts the cells and what a pass left in them, by owner.
    pub(super) fn report_memory(&self, report: &mut MemoryReport) {
        let pages = self.pages.iter().filter_map(std::sync::OnceLock::get);
        report.add(
            MemoryOwner::RecordCells,
            crate::heap_size::vec_bytes(&self.pages) + pages.clone().count() * size_of::<CellPage>(),
        );
        for page in pages {
            for held in page.held.iter().filter_map(WriteOnce::get) {
                held.report_memory(report);
            }
            for records in page.pseudo_element_records.iter().filter_map(WriteOnce::get) {
                report_pseudo_element_records_memory(records, report);
            }
        }
    }

    /// Make room for `slot_count` elements.
    pub(super) fn reserve(&mut self, slot_count: usize) {
        self.slot_count = self.slot_count.max(slot_count);
        let pages = slot_count.div_ceil(CELL_PAGE_SIZE);
        if self.pages.len() < pages {
            self.pages.resize_with(pages, std::sync::OnceLock::new);
        }
    }

    fn page(&self, slot: usize) -> Option<&CellPage> {
        self.pages.get(slot / CELL_PAGE_SIZE)?.get().map(|page| &**page)
    }

    /// The page holding the cells of the element in `slot`, to write them, made where it is not.
    fn to_write(&self, slot: usize) -> &CellPage {
        self.pages[slot / CELL_PAGE_SIZE].get_or_init(Box::default)
    }

    fn held(&self, slot: usize) -> Option<&Held> {
        self.page(slot)?.held[slot % CELL_PAGE_SIZE].get()
    }

    fn pseudo_element_records(&self, slot: usize) -> Option<&PseudoElementRecords> {
        self.page(slot)?.pseudo_element_records[slot % CELL_PAGE_SIZE].get()
    }

    fn container_reads(&self, slot: usize) -> Option<&Arc<ContainerVerdict>> {
        self.page(slot)?.container_reads[slot % CELL_PAGE_SIZE].get()
    }

    /// Lets go of the pages the cells were written to.
    fn release(&mut self) {
        for page in &mut self.pages {
            page.take();
        }
    }
}

/// The records of a pass, falling back to the records the passes before kept, and then to the
/// installed record of an element no pass computed, so its descendants still compute against a
/// style.
///
/// An element whose animations the host composes over the record it holds hands its children the
/// composition: they inherit the animated values.
#[derive(Clone, Copy)]
pub(super) struct Records<'a> {
    state: &'a RetainedState,
    cells: &'a RecordCells,
    previous: Option<&'a RetainedRecords>,
    /// What says that the record an element holds and its record here hold equal values, where
    /// it then hands its children the composition the host lays over the record it holds, as one
    /// a reference pass computed anew does, custom property stores of its own included.
    composes_over_equal_records: Option<fn(&Record, &Record) -> bool>,
}

impl<'a> Records<'a> {
    /// The root font inputs the last pass that styled the document element `node` resolved
    /// against, where one did.
    pub(super) fn kept_root_font(&self, node: StyleNodeID) -> Option<super::compute::RootFontInputs> {
        self.previous?.kept_root_font(node)
    }

    pub(super) fn new(state: &'a RetainedState, cells: &'a RecordCells, previous: Option<&'a RetainedRecords>) -> Self {
        Self {
            state,
            cells,
            previous,
            composes_over_equal_records: None,
        }
    }

    /// These records, handing children the composition over a record the host holds that holds
    /// values equal to an element's record here, as `equal` says.
    pub(super) fn composing_over_equal_records(self, equal: fn(&Record, &Record) -> bool) -> Self {
        Self {
            composes_over_equal_records: Some(equal),
            ..self
        }
    }

    /// The view of the composition of the animations of `node` over the record it holds, where the
    /// host composes them over that record, values of running transitions included: a child
    /// inherits them as it inherits any animated value.
    ///
    /// NB: A transition of the child decides against the after-change style, which inherits the
    ///     parent's after-change style: the decision takes the value beneath the transitions of
    ///     the ancestor the child inherits it from, as the records mark the values they inherit.
    pub(super) fn composition(&self, node: StyleNodeID) -> Option<computed::StyleRecordView<'a>> {
        let sets = &self.state.computed_group_sets;
        let record = self.get_shared(node)?;
        if let Some(view) = sets.composition_over_record(node, record) {
            return Some(view);
        }
        let equal = self.composes_over_equal_records?;
        let held = sets.held_record(node).filter(|held| equal(held, record))?;
        sets.composition_over_record(node, held)
    }

    /// The custom properties `node` hands down and the ones it holds, here or as it is installed,
    /// which an inheriting lookup reads: those its animations sampled, where the host sampled them
    /// over the environment of the record it holds.
    pub(super) fn custom_stores_of(
        &self,
        node: StyleNodeID,
    ) -> Option<(BorrowedCustomProperties<'a>, BorrowedCustomProperties<'a>)> {
        let sets = &self.state.computed_group_sets;
        let record = match self.get_shared(node) {
            Some(record) => record,
            None => sets.installed_base_record(sets.assigned_style_record(node)?)?,
        };
        // NB: Only an element whose animations sampled custom properties asks whether its record
        //     holds the ones they were sampled over, which compares the records.
        if let Some(sampled) = self.sampled_custom_properties(node)
            && self.samples_custom_properties_of(node, record)
        {
            return Some((
                self.state
                    .inheritable_custom_property_store(sampled.as_ref(), None)
                    .map(|store| Cow::Owned(store.into_owned())),
                sampled.map(Cow::Owned),
            ));
        }
        Some((
            record.inheritable_custom_properties().map(Cow::Borrowed),
            record.custom_properties().map(Cow::Borrowed),
        ))
    }

    /// Whether the custom properties the host sampled the animations of `node` over are the ones
    /// `record`, which this pass may have computed, holds: those of the record the element holds,
    /// where `record` holds the same custom properties as it, as a store computed again does. A pseudo-element computed in the
    /// same pass as a new record of its element that moved no custom property inherits the
    /// animated values as it did.
    fn samples_custom_properties_of(&self, node: StyleNodeID, record: &Arc<Record>) -> bool {
        let sets = &self.state.computed_group_sets;
        if sets.holds_record(node, record) {
            return true;
        }
        // NB: A cold pass builds its stores anew, chained differently from those an element kept across its updates. The
        //     values are what it compares against the record the host sampled over, as it does for the composition.
        if let Some(equal) = self.composes_over_equal_records
            && sets.held_record(node).is_some_and(|held| equal(held, record))
        {
            return true;
        }
        let same_store = crate::css::custom_properties::CustomPropertyStore::absent_or_holding_the_same_values;
        sets.held_record(node).is_some_and(|held| {
            same_store(held.custom_properties(), record.custom_properties())
                && same_store(
                    held.inheritable_custom_properties(),
                    record.inheritable_custom_properties(),
                )
        })
    }

    /// The custom properties the animations of `node` sampled, where the host sampled them over
    /// the environment of the record the element holds: the element's own custom properties with
    /// the animated values in place of theirs.
    pub(super) fn sampled_custom_properties(&self, node: StyleNodeID) -> Option<HeldCustomProperties> {
        self.state
            .sampled_custom_property_store(node)
            .map(|store| store.cloned())
    }

    /// The custom properties the animations of the pseudo-element of `kind` of `node` sampled:
    /// its own custom properties with the animated values in place of theirs, where the host
    /// holds an animation overlay for it.
    pub(super) fn sampled_pseudo_element_custom_properties(&self, node: StyleNodeID, kind: u8) -> HeldCustomProperties {
        let (_, held) = self
            .state
            .pseudo_element_custom_property_data
            .get(&node)?
            .iter()
            .find(|(pseudo, held)| *pseudo == kind && held.sampled_over.is_some())?;
        self.state.custom_property_environments.store(held.identity).cloned()
    }

    /// The record `node` held before this pass, or the one the host holds for it or composes its animations over, where
    /// it holds the style `record` does: what it keeps in place of `record`, so that its children compute from it as
    /// they did, and the host has nothing to take.
    pub(super) fn held_before_holding_the_same_style_as(
        &self,
        node: StyleNodeID,
        record: &Record,
    ) -> Option<&'a Arc<Record>> {
        let previous = self.previous?.get(node);
        let held = self
            .state
            .held_style_records
            .get(node)
            .map(|held| held.sample_of_animations().map_or(held, |sample| &sample.base));
        previous
            .into_iter()
            .chain(held.filter(|held| previous.is_none_or(|previous| !Arc::ptr_eq(previous, held))))
            .find(|candidate| candidate.holds_the_same_style_as(record))
    }

    /// Whether this pass computed `node`.
    pub(super) fn computed(&self, node: StyleNodeID) -> bool {
        self.cells.held(node.element_slot()).is_some()
    }

    /// Whether `node` holds a record the host does not hold yet: one this pass computed, or one a
    /// pass before it computed that was not installed since, as where a pass restyles only the
    /// elements that waited on a font or a random base value. The elements below it read it as
    /// that record has it.
    fn holds_uninstalled_record(&self, node: StyleNodeID) -> bool {
        if self.computed(node) {
            return true;
        }
        let Some(record) = self.previous.and_then(|previous| previous.get(node)) else {
            return false;
        };
        self.state
            .computed_group_sets
            .held_record(node)
            .is_none_or(|held| !Arc::ptr_eq(held, record) && !held.holds_the_same_style_as(record))
    }

    /// Which children of `node` the record this pass computed for it restyles, as
    /// [`RetainedState::child_impact_of_records`] finds of the one it held before, or all of them
    /// where its highlight pseudo-elements moved.
    pub(super) fn child_impact(&self, node: StyleNodeID) -> ChildImpact {
        let (Some(new), Some(old)) = (
            self.cells.held(node.element_slot()),
            self.previous.and_then(|previous| previous.get(node)),
        ) else {
            return ChildImpact::All;
        };
        // https://drafts.csswg.org/css-pseudo-4/#highlight-cascade
        // The highlight pseudo-elements of the children inherit from the element's.
        if self.highlights_moved(node) {
            return ChildImpact::All;
        }
        self.state.child_impact_of_records(node, &new.record, old)
    }

    /// Whether the records this pass computed for the highlight pseudo-elements of `node` hold
    /// other styles than the ones it held before.
    fn highlights_moved(&self, node: StyleNodeID) -> bool {
        let Some(new) = self.cells.pseudo_element_records(node.element_slot()) else {
            return false;
        };
        let old = self
            .previous
            .map_or(&[][..], |previous| previous.pseudo_element_records(node));
        highlights_moved(new, old)
    }

    pub(super) fn get(&self, node: StyleNodeID) -> Option<&'a Record> {
        self.get_shared(node).map(|record| &**record)
    }

    /// The records of the pseudo-elements of `node` that generate a box, by target.
    pub(super) fn pseudo_element_records(&self, node: StyleNodeID) -> &'a [(PseudoElementTarget, Arc<Record>)] {
        match self.cells.pseudo_element_records(node.element_slot()) {
            Some(records) => records.as_slice(),
            None if self.computed(node) => &[],
            None => self
                .previous
                .map_or(&[], |previous| previous.pseudo_element_records(node)),
        }
    }

    pub(super) fn insert_pseudo_element_records(&self, node: StyleNodeID, records: PseudoElementRecords) {
        for (_, record) in &records {
            self.note_sibling_position_read(node, record);
        }
        assert!(
            self.cells.to_write(node.element_slot()).pseudo_element_records[node.element_slot() % CELL_PAGE_SIZE]
                .set(records)
                .is_ok(),
            "an element's pseudo-elements are styled once per pass"
        );
    }

    fn held(&self, node: StyleNodeID) -> Option<&'a Held> {
        match self.cells.held(node.element_slot()) {
            Some(held) => Some(held),
            None => self.previous?.held(node),
        }
    }

    pub(super) fn get_shared(&self, node: StyleNodeID) -> Option<&'a Arc<Record>> {
        Some(&self.held(node)?.record)
    }

    /// The identity of the elements `node` is indistinguishable from to selectors: the element
    /// that was computed, which every element sharing its style through the sharing cache names.
    ///
    /// NB: An identity holds for the pass that shared the style: what no rule reads can change on an
    ///     element no pass restyles, so one the pass did not style is alike to none but itself.
    pub(super) fn sharing_identity(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match self.cells.held(node.element_slot()) {
            Some(held) => Some(held.sharing_identity),
            None => self.previous?.held(node).map(|_| node),
        }
    }

    /// Keep the record `node` held before for this pass, with what it matched.
    pub(super) fn keep_previous(&self, node: StyleNodeID) -> bool {
        let matches = self.previous.and_then(|previous| previous.held(node)?.matches.clone());
        self.keep_previous_matching(node, matches)
    }

    /// Keep what `node` held before, but for the entries it matched, which are `matches` now.
    pub(super) fn keep_previous_matching(&self, node: StyleNodeID, matches: Option<MatchList>) -> bool {
        let Some(previous) = self.previous else {
            return false;
        };
        let Some(held) = previous.held(node) else {
            return false;
        };
        self.insert(node, held.record.clone(), node, matches);
        self.insert_pseudo_element_records(node, previous.pseudo_element_records(node).into());
        if let Some(reads) = previous.container_reads(node) {
            self.insert_container_reads(node, reads.clone());
        }
        true
    }

    /// Keep `record`, the one `node` held before over the custom properties its parent hands down
    /// now, with what else it held.
    pub(super) fn keep_previous_over_moved_custom_properties(&self, node: StyleNodeID, record: Arc<Record>) -> bool {
        let Some(held) = self.previous.and_then(|previous| previous.held(node)) else {
            return false;
        };
        self.insert(node, record, node, held.matches.clone());
        self.insert_pseudo_element_records(node, PseudoElementRecords::new());
        if let Some(reads) = self.previous.and_then(|previous| previous.container_reads(node)) {
            self.insert_container_reads(node, reads.clone());
        }
        true
    }

    /// Note on the parent of `node` that `record`, held for it or a pseudo-element of it, computes
    /// from where it stands among its siblings, which an element moving among them moves.
    fn note_sibling_position_read(&self, node: StyleNodeID, record: &Record) {
        if record.rare().reads.reads_sibling_position {
            self.state.projection.note_structural_read(
                TransactionFactSide::After,
                node,
                StructuralRead::SiblingPositionValue,
            );
        }
    }

    /// Keep what the container conditions of the rules `node` matched read of its containers.
    pub(super) fn insert_container_reads(&self, node: StyleNodeID, reads: Arc<ContainerVerdict>) {
        assert!(
            self.cells.to_write(node.element_slot()).container_reads[node.element_slot() % CELL_PAGE_SIZE]
                .set(reads)
                .is_ok(),
            "an element is styled once per pass"
        );
    }

    /// What the container conditions of the rules `node` matched read of its containers, here
    /// or in a pass before, where it matched any rule applying under container conditions.
    pub(super) fn container_reads(&self, node: StyleNodeID) -> Option<&Arc<ContainerVerdict>> {
        match self.computed(node) {
            true => self.cells.container_reads(node.element_slot()),
            false => self.previous?.container_reads(node),
        }
    }

    /// How much of the root element's font the style of `node` resolves lengths against, its own
    /// or its pseudo-elements', or through the container conditions of the rules it matched, whose
    /// queries may read any of it, as may a node having no record.
    pub(super) fn reads_root_font(&self, node: StyleNodeID) -> RootFontReads {
        let Some(record) = self.get(node) else {
            return RootFontReads::Metrics;
        };
        if self.container_reads(node).is_some_and(|reads| reads.reads_root_font) {
            return RootFontReads::Metrics;
        }
        self.pseudo_element_records(node)
            .iter()
            .fold(record.rare().reads.reads_root_font, |reads, (_, record)| {
                reads.max(record.rare().reads.reads_root_font)
            })
    }

    /// Whether `node` reads its containers, through the container conditions of the rules it
    /// matched or the container-relative lengths of its own or its pseudo-elements' styles.
    pub(super) fn reads_containers(&self, node: StyleNodeID) -> bool {
        self.container_reads(node).is_some()
            || self
                .get(node)
                .is_some_and(|record| record.rare().reads.container_units != 0)
            || self
                .pseudo_element_records(node)
                .iter()
                .any(|(_, record)| record.rare().reads.container_units != 0)
    }

    /// Whether the record this pass computed for `node` moves beneath a composition of its
    /// animations, which the host lays over the new record once it is installed: the element has
    /// animations, or the record it moves to or from declares CSS animations, which the host
    /// plans. A transition the host starts once it installs the record composes nothing the
    /// children wait for: they compute from the after-change style, and restyle once the host
    /// publishes the values the transition starts from.
    pub(super) fn moves_beneath_a_composition(&self, node: StyleNodeID) -> bool {
        let Some(new) = self.cells.held(node.element_slot()) else {
            return false;
        };
        let new = &new.record;
        let old = self.previous.and_then(|previous| previous.get(node));
        if old.is_some_and(|old| Arc::ptr_eq(old, new)) {
            return false;
        }
        let animates = |record: &Record| record.facts().declares_css_animations();
        (animates(new)
            || old.is_some_and(|old| animates(old))
            || self.state.projection.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0)
            && old.is_none_or(|old| !new.holds_the_same_style_as(old))
    }

    /// Whether the record this pass computed for `node` is displayed where the one it held before
    /// was not: the elements below it were not styled since, and match and compute anew.
    pub(super) fn revealed(&self, node: StyleNodeID) -> bool {
        let Some(new) = self.cells.held(node.element_slot()) else {
            return false;
        };
        self.previous
            .and_then(|previous| previous.get(node))
            .is_some_and(|old| !old.is_displayed() && new.record.is_displayed())
    }

    /// Whether the record this pass computed for `node` describes another container than the one
    /// it held before: another query container, by type, name, writing mode or direction, or,
    /// where `custom_properties` asks, a style container holding other custom properties. The
    /// elements below it that read their containers read them anew.
    pub(super) fn container_moved(&self, node: StyleNodeID, custom_properties: bool) -> bool {
        let (Some(new), Some(old)) = (
            self.cells.held(node.element_slot()),
            self.previous.and_then(|previous| previous.get(node)),
        ) else {
            return false;
        };
        let new = &new.record;
        if Arc::ptr_eq(new, old) {
            return false;
        }
        let state = self.state;
        fn row<'r>(
            state: &RetainedState,
            node: StyleNodeID,
            record: &'r Record,
        ) -> Option<super::super::tree::ContainerQueryInputs<'r>> {
            state.container_query_input_row_of_payloads(node, &record.payloads, 0, true)
        }
        // A size container's font-relative query lengths (`@container (width > 5em)`) resolve
        // against its own font and line height, which its row does not hold.
        let font_moved = || {
            let font = group_index::FONT;
            let box_values = crate::css::computed_value_views::ComputedValuesView::of(&new.payloads).box_values();
            (box_values.is_size_container || box_values.is_inline_size_container)
                && new.payloads[font].as_ptr() != old.payloads[font].as_ptr()
        };
        row(state, node, new) != row(state, node, old)
            || font_moved()
            || (custom_properties
                && match (new.custom_properties(), old.custom_properties()) {
                    (None, None) => false,
                    (Some(a), Some(b)) => !Arc::ptr_eq(a, b),
                    _ => true,
                })
    }

    /// The rule index entries `node` matched, here or in a pass before, where they were kept.
    pub(super) fn matches_of(&self, node: StyleNodeID) -> Option<&MatchList> {
        self.held(node)?.matches.as_ref()
    }

    pub(super) fn insert(
        &self,
        node: StyleNodeID,
        record: Arc<Record>,
        sharing_identity: StyleNodeID,
        matches: Option<MatchList>,
    ) {
        self.note_sibling_position_read(node, &record);
        assert!(
            self.cells.to_write(node.element_slot()).held[node.element_slot() % CELL_PAGE_SIZE]
                .set(Held {
                    record,
                    sharing_identity,
                    matches,
                })
                .is_ok(),
            "an element is styled once per pass"
        );
    }

    /// The resolved color of the style `node` holds, here or as it is installed.
    pub(super) fn current_color_of(&self, node: StyleNodeID) -> Option<u32> {
        let view = match self.get(node) {
            Some(record) => match self.composition(node) {
                Some(view) => view,
                None => return Some(record.payloads.current_color()),
            },
            None => self.state.assigned_record_view(node)?,
        };
        let payload = view.payloads[crate::css::table_group_builder::group_index::INHERITED_TEXT].as_ptr();
        if payload.is_null() {
            return None;
        }
        // SAFETY: The payload is the live inherited text group of the record.
        Some(unsafe { &*payload.cast::<crate::css::computed_value_types::InheritedTextValues>() }.color)
    }
}

impl ContainerRecords for Records<'_> {
    /// An element holding a record the host does not hold yet, which this pass or one before it in
    /// the same update computed, is the query container that record says it is, which the
    /// elements below it compute against before the record is installed.
    fn query_row(&self, node: StyleNodeID) -> Option<super::super::tree::ContainerQueryInputs<'_>> {
        if !self.holds_uninstalled_record(node) {
            return self.state.query_row(node);
        }
        let record = self.get(node)?;
        self.state
            .container_query_input_row_of_payloads(node, &record.payloads, 0, false)
    }

    fn any_element_row(&self, node: StyleNodeID) -> Option<super::super::tree::ContainerQueryInputs<'_>> {
        if !self.holds_uninstalled_record(node) {
            return self.state.any_element_row(node);
        }
        let record = self.get(node)?;
        self.state
            .container_query_input_row_of_payloads(node, &record.payloads, 0, true)
    }

    fn record_view(
        &self,
        node: StyleNodeID,
        row: super::super::tree::ContainerQueryInputs<'_>,
    ) -> Option<computed::StyleRecordView<'_>> {
        match self.holds_uninstalled_record(node) {
            true => self.assigned_record_view(node),
            false => self.state.record_view(node, row),
        }
    }

    /// The custom properties the element's record holds, with the values the animations of an
    /// element still holding it sampled in place of theirs, as its children inherit them.
    fn custom_store(
        &self,
        node: StyleNodeID,
        row: super::super::tree::ContainerQueryInputs<'_>,
    ) -> Option<Option<&crate::css::custom_properties::CustomPropertyStore>> {
        if !self.holds_uninstalled_record(node) {
            return self.state.custom_store(node, row);
        }
        let record = self.get_shared(node)?;
        if self.samples_custom_properties_of(node, record)
            && let Some(sampled) = self.state.sampled_custom_property_store(node)
        {
            return Some(sampled.map(|store| &**store));
        }
        Some(record.custom_properties().map(|store| &**store))
    }
}

impl Record {
    /// Asks for the memory of the fields a thread first reads of a record another thread made.
    pub(super) fn prefetch(&self) {
        crate::css::declaration_block::prefetch(std::ptr::from_ref(&self.payloads));
        crate::css::declaration_block::prefetch(std::ptr::from_ref(&self.header));
    }

    /// The view of the record, as the computation of the elements inheriting from it reads it.
    pub(in super::super) fn view(self: &Arc<Self>) -> computed::StyleRecordView<'_> {
        computed::StyleRecordView {
            payloads: &self.payloads,
            base_payloads: &self.payloads,
            record: self,
            animated_overlay: None,
            pseudo_element_styles: 0,
            counter_style_environment_identity: 0,
            dependency_flags: self.facts().publication_dependency_flags(),
        }
    }
}

impl<'a> Records<'a> {
    /// The record the highlight pseudo-element `target` of `node` inherits from: that of the same
    /// pseudo-element of its nearest flat-tree ancestor holding one, here or, for an ancestor these
    /// records hold nothing for, as it is installed.
    pub(super) fn highlight_inheritance_parent(
        &self,
        node: StyleNodeID,
        target: PseudoElementTarget,
    ) -> Option<HighlightParent<'a>> {
        let mut ancestor = self.state.projection.inheritance_parent(node);
        while let Some(current) = ancestor {
            match self.get(current) {
                Some(_) => {
                    if let Some((_, record)) = self
                        .pseudo_element_records(current)
                        .iter()
                        .find(|(held, _)| *held == target)
                    {
                        return Some(HighlightParent::Record(record));
                    }
                }
                None => {
                    if let Some(record) = self.state.installed_pseudo_style_record(current, target) {
                        return Some(HighlightParent::Installed(record));
                    }
                }
            }
            ancestor = self.state.projection.inheritance_parent(current);
        }
        None
    }
}

/// The record a highlight pseudo-element inherits from: one the engine computed, or one the current
/// engine installed.
#[derive(Clone, Copy)]
pub(super) enum HighlightParent<'a> {
    Record(&'a Arc<Record>),
    Installed(computed::FinalStyleRecordID),
}

impl DriveRecords for Records<'_> {
    fn assigned_record_view(&self, node: StyleNodeID) -> Option<computed::StyleRecordView<'_>> {
        let Some(record) = self.get_shared(node) else {
            return self.state.assigned_record_view(node);
        };
        if let Some(view) = self.composition(node) {
            return Some(view);
        }
        Some(record.view())
    }

    fn highlight_inheritance_parent_view(
        &self,
        node: StyleNodeID,
        target: PseudoElementTarget,
    ) -> Option<computed::StyleRecordView<'_>> {
        match self.highlight_inheritance_parent(node, target)? {
            HighlightParent::Record(record) => Some(record.view()),
            HighlightParent::Installed(record) => self.state.computed_group_sets.style_record_view(record.raw()),
        }
    }
}
