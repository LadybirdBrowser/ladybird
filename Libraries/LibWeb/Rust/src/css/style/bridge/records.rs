/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The record the host holds for a base or live animation-overlay record, which the engine holds while it holds the
/// identity, or null for an identity it does not hold. The host counts a reference to it to hold it longer.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_host_record(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    style_record: u64,
) -> *const c_void {
    if let Some(record) = crate::css::style::engine_calls::known_host_record(host, style_record) {
        return record;
    }
    let record = with_engine(read, host, |engine| host_record(engine, style_record));
    if !record.is_null() {
        host.engine_memo().views.set(style_record, record);
    }
    record
}

/// The record `engine` holds for the host for `style_record`, or null.
pub(in crate::css::style) fn host_record(engine: &StyleEngine, style_record: u64) -> *const c_void {
    engine
        .computed_group_sets
        .host_record(style_record)
        .map_or(std::ptr::null(), |record| std::sync::Arc::as_ptr(record).cast())
}

/// The groups beneath the animation overlay of the record `view` views, which are its own groups
/// where it carries none, or null where the view is missing.
///
/// # Safety
/// `view` must be a view of a record the host holds live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_view_base_groups(view: &FfiStyleRecordView) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { view.base_groups() }.map_or(std::ptr::null(), |groups| std::ptr::from_ref(groups).cast())
}

/// Whether the element of the record `view` views would compute `display` to `none` when ignoring its animations:
/// those composed over the record, and the custom properties they sampled, which the record substitutes.
///
/// # Safety
/// `view` must be a view of a record the host holds live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_view_display_is_none_ignoring_animations(view: &FfiStyleRecordView) -> bool {
    // SAFETY: Guaranteed by the caller.
    let Some(groups) = (unsafe { view.base_groups() }) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    let display_none_is_animated = unsafe {
        view.longhand_table_record
            .cast::<crate::css::style::pipeline::Record>()
            .as_ref()
    }
    .is_some_and(|record| record.rare().reads.display_none_is_animated);
    crate::css::computed_value_views::ComputedValuesView::of(groups)
        .display()
        .is_none()
        && !display_none_is_animated
}

/// The view of `record`, a record the host holds, which reads nothing but the record.
///
/// # Safety
/// `record` must be a record the caller holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_view(record: *const c_void) -> FfiStyleRecordView {
    // SAFETY: Guaranteed by the caller.
    FfiStyleRecordView::of_record(unsafe { &*record.cast::<crate::css::style::pipeline::Record>() })
}

/// The identity the engine names `record`, a record the host holds, by: that of the slot holding
/// it, or of the sample it is, or zero for a record no slot holds any longer.
///
/// # Safety
/// As for [`style_engine_record_view`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_identity(record: *const c_void) -> u64 {
    // SAFETY: Guaranteed by the caller.
    host_record_identity(unsafe { &*record.cast::<crate::css::style::pipeline::Record>() })
}

/// The identity the engine names `record`, a record the host holds, by, as
/// [`style_engine_record_identity`] answers it.
pub(in crate::css::style) fn host_record_identity(record: &crate::css::style::pipeline::Record) -> u64 {
    match record.sample_of_animations() {
        Some(sample) => sample.identity.raw(),
        None => record.installed_record().map_or(0, |installed| {
            crate::css::style::computed::FinalStyleRecordID::installed(installed).raw()
        }),
    }
}

/// The identity of the custom-property environment `record`, a record the host holds, was
/// resolved in: that of the store it holds, or of the store of the record a sample samples over.
///
/// # Safety
/// As for [`style_engine_record_view`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_custom_property_environment(record: *const c_void) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let record = unsafe { &*record.cast::<crate::css::style::pipeline::Record>() };
    record
        .sample_of_animations()
        .map_or(record, |sample| &sample.base)
        .custom_property_environment()
}

/// Takes one more reference to `record`, a style record a view named.
///
/// # Safety
/// `record` must be the `longhand_table_record` of a view of a record the host holds live, or one
/// the caller holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_retain(record: *const c_void) {
    // SAFETY: Guaranteed by the caller: the record lives in an Arc.
    unsafe { std::sync::Arc::increment_strong_count(record.cast::<crate::css::style::pipeline::Record>()) };
}

/// A reference of the engine's own to `record`, a record the host holds, or none for null.
///
/// # Safety
/// `record` must be null or a record the host holds a reference to.
pub(crate) unsafe fn retain_host_record(
    record: *const c_void,
) -> Option<std::sync::Arc<crate::css::style::pipeline::Record>> {
    let record = record.cast::<crate::css::style::pipeline::Record>();
    // SAFETY: Guaranteed by the caller: the record lives in an Arc.
    (!record.is_null()).then(|| unsafe {
        std::sync::Arc::increment_strong_count(record);
        std::sync::Arc::from_raw(record)
    })
}

/// Lets go of a reference taken with `style_engine_record_retain`.
///
/// # Safety
/// `record` must be a record the caller took a reference to with `style_engine_record_retain`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_release(record: *const c_void) {
    // SAFETY: Guaranteed by the caller.
    unsafe { std::sync::Arc::decrement_strong_count(record.cast::<crate::css::style::pipeline::Record>()) };
}

/// Whether the `transition-*` values of the record `view` views give any longhand a matching
/// entry, as [`crate::css::transition::has_matching_transition_entries`] says.
///
/// # Safety
/// `view` must be a view of a live record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_view_has_matching_transition_entries(
    view: &FfiStyleRecordView,
    has_existing_transitions: bool,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { view.longhands() }.is_some_and(|longhands| {
        crate::css::transition::has_matching_transition_entries(longhands, has_existing_transitions)
    })
}

/// What a read of one element's style the host makes before the next style update asks of the
/// engine; see `StyleEngine::answer_record_demand`. A read-only demand leaves the engine as it
/// was: its record is only for the host to read. Any other is installed and acknowledged as a
/// style update's would be.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
#[expect(clippy::enum_variant_names, reason = "each is a read of the element")]
pub enum FfiRecordDemand {
    /// A read-only read of the element's record.
    ElementRead,
    /// A read-only read of what the element computes to as though it had no inline declaration,
    /// driven in full against the parent as it is now.
    ElementReadWithoutInlineStyle,
    /// A read-only read of the element's record, driven in full against the parent as it is now,
    /// which the reader assigned a record of its own.
    ElementReadAgainstParent,
}

/// What a read of one of an element's pseudo-elements the host makes before the next style update
/// asks of the engine, as `FfiRecordDemand` does of an element.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiPseudoElementRecordDemand {
    /// A CSSOM read of the pseudo-element, settled against its element's installed record.
    CssomRead,
    /// A read-only read of what the pseudo-element computes to, whether or not it generates a box.
    ReadOnly,
}

/// A pseudo-element a record demand may read: a synthetic one, which the engine settles beside its
/// element, or, for a read-only read, one an element in the shadow tree backs or a named view
/// transition one, which it computes from the element's rules for it. Each is numbered as its kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiDemandedPseudoElement {
    After = 0,
    Backdrop = 1,
    Before = 2,
    FirstLetter = 3,
    FirstLine = 4,
    Marker = 5,
    SearchText = 6,
    SearchTextCurrent = 7,
    Selection = 8,
    SpellingError = 9,
    ViewTransition = 10,
    WebkitColorSwatchWrapper = 11,
    ColorSwatch = 12,
    DetailsContent = 13,
    FileSelectorButton = 14,
    Placeholder = 15,
    SliderFill = 16,
    SliderThumb = 17,
    SliderTrack = 18,
    Highlight = 19,
    ViewTransitionGroup = 22,
    ViewTransitionImagePair = 23,
    ViewTransitionNew = 24,
    ViewTransitionOld = 25,
}

/// The answer to a record demand: the record, or that the pseudo-element read generates no box
/// with the custom-property environment its rules resolve to (zero where none styles it). A zero
/// `style_record` that is not absent leaves the read to C++.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiRecordDemandAnswer {
    pub record: FfiEngineComputedRecord,
    pub is_absent: bool,
    pub custom_property_environment: u64,
}

/// Answer a read of one element's style, or one of its pseudo-elements', the host makes before
/// the next style update.
pub(crate) fn answer_record_demand(
    engine: &mut StyleEngine,
    style_node: StyleNodeID,
    demand: crate::css::style::publication::RecordDemand,
) -> FfiRecordDemandAnswer {
    let answer = engine.answer_record_demand(style_node, demand);
    ffi_record_demand_answer(engine, answer)
}

/// The FFI form of `answer`, what a demand of an element's record answered, with the records
/// `engine` holds for the host for the records it names.
pub(in crate::css::style) fn ffi_record_demand_answer(
    engine: &StyleEngine,
    answer: crate::css::style::publication::RecordDemandAnswer,
) -> FfiRecordDemandAnswer {
    match answer {
        crate::css::style::publication::RecordDemandAnswer::Record { record } => FfiRecordDemandAnswer {
            record: FfiEngineComputedRecord {
                style_record: record.style_record,
                host_record: host_record(engine, record.style_record),
            },
            is_absent: false,
            custom_property_environment: 0,
        },
        crate::css::style::publication::RecordDemandAnswer::Absent {
            custom_property_environment,
        } => FfiRecordDemandAnswer {
            record: FfiEngineComputedRecord::default(),
            is_absent: true,
            custom_property_environment,
        },
    }
}

/// The record of an element no rule reaches, computed from its presentational hints and its
/// inline style alone over the initial values, its custom properties among them; see
/// `declared_only_record`. `subject` is the document's style node. Returns the record with a
/// reference the host takes over, or null where the engine computes none.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. `hints` must borrow `hint_count`
/// `FfiDeclaredProperty` entries whose values point at live, Arc-backed `StyleValueData` roots. A
/// non-null `inline_block` must borrow a live `DeclarationBlock`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_declared_only_record(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    subject: u32,
    facts: u32,
    hint_kind: FfiElementDeclarationKind,
    hints: *const c_void,
    hint_count: usize,
    inline_block: *const c_void,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    let record = with_engine(read, host, |engine| {
        use crate::css::declaration_block::{DeclarationBlock, FfiDeclaredProperty, declaration_from_view};
        abort_on_panic(|| {
            let subject = StyleNodeID::from_raw(subject)?;
            let hints = if hint_count == 0 {
                &[]
            } else {
                unsafe { ak::slice_from_raw_parts(hints.cast::<FfiDeclaredProperty>(), hint_count) }
            };
            let hints = hints
                .iter()
                .map(|hint| unsafe { declaration_from_view(hint) })
                .collect::<Vec<_>>();
            let inline_data = unsafe { inline_block.cast::<DeclarationBlock>().as_ref() }.map(DeclarationBlock::data);
            let hint_kind = decode_element_declaration_kind(hint_kind);
            let mut by_kind: [Vec<_>; ElementDeclarationKind::COUNT] = Default::default();
            by_kind[hint_kind.index()] = crate::css::style::specified_value::element_declared_properties(
                hints
                    .iter()
                    .map(crate::css::declaration_block::DeclaredProperty::borrowed),
            );
            if let Some(data) = &inline_data {
                by_kind[ElementDeclarationKind::InlineStyle.index()] =
                    crate::css::style::specified_value::element_declared_properties(data.properties().iter());
            }
            let custom_declarations = collect_native_custom_declarations(
                engine,
                inline_data
                    .as_ref()
                    .map_or(Default::default(), |data| data.custom_properties()),
            );
            let declared = crate::css::style::pipeline::DeclaredOnly {
                declarations: crate::css::style::element_declarations::ElementDeclarationRow::new(
                    by_kind,
                    custom_declarations,
                ),
                // An element outside the document is no document element.
                facts: facts & !element_adjustment_fact::IS_DOCUMENT_ELEMENT,
            };
            // FIXME: A shape in a window whose document has no document element has a path and should measure, but an
            //        engine owns its document from its first style update with a document element, so no initial
            //        style exists yet to compute the record over.
            if !engine.has_live_style() {
                return None;
            }
            engine.declared_only_record(subject, declared)
        })
    });
    record.map_or(std::ptr::null(), |record| std::sync::Arc::into_raw(record).cast())
}

/// The store of an environment the engine resolved, with one strong reference transferred to the
/// caller, and the environment it was resolved over; null for an environment C++ published.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `parent` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_borrow_engine_custom_property_environment(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    identity: u64,
    parent: *mut u64,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    with_engine(read, host, |engine| {
        let Some((store, parent_identity)) = engine.custom_property_environments.engine_environment(identity) else {
            return std::ptr::null();
        };
        unsafe { *parent = parent_identity };
        std::sync::Arc::into_raw(std::sync::Arc::clone(store)).cast()
    })
}
