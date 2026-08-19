/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Builds computed style group payloads directly from a drive's computed
//! longhand table.
//!
//! One call replaces the per-group marshalling loops the C++ style consumer
//! used to run: for every applied group it gathers the group's values from
//! the table (with a sparse override span for effective values the table does
//! not hold, like the animated overlay), resolves the color and opacity
//! sidecars natively, and runs the existing group builders, so the sharing
//! rules - adopt the parent's payload on equality, fall back to the immortal
//! default payload - are exactly the ones the marshalled calls used. A group
//! whose values the core cannot map stays null in the output and is reported
//! in the returned mask for the C++ population path.

use std::collections::HashMap;
use std::ffi::c_void;

use crate::abort_on_panic;
use crate::css::calc::{resolve_calculated_flex_without_context, resolve_calculated_integer_without_context};
use crate::css::color_resolution::{
    ColorResolutionInput, FfiColorResolutionInput, PREFERRED_COLOR_SCHEME_DARK, Rgba, accent_color,
    relative_color_context_from_ffi, resolution_input_from_ffi, to_color,
};
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::computed_value_types::{
    AnchorValues, AnimationValues, BackgroundValues, BorderValues, ComputedClipEdge, ComputedColorOrAuto,
    ComputedCursor, ComputedFilter, ComputedFilterOperation, ComputedGridArea, ComputedGridPlacement,
    ComputedGridPlacementKind, ComputedGridTrackBreadth, ComputedGridTrackEntry, ComputedGridTrackEntryKind,
    ComputedGridTrackList, ComputedLengthBox, ComputedOverflowClipMargin, ComputedOverflowClipMarginSide,
    ComputedPositionTryFallback, ComputedResolvedTransform, ComputedScrollbarColor, ComputedShadow, ComputedSize,
    ComputedSizeKind, ComputedStyleValueHandle, ComputedSvgDash, ComputedSvgPaint, ComputedTextIndent,
    ComputedTextUnderlineOffset, ComputedTextUnderlinePosition, ContentValues, EffectsValues, FontValues,
    GRID_NO_INDEX, GridValues, InheritedListValues, InheritedSVGValues, InheritedTextValues, InheritedUIValues,
    MaskValues, MiscResetValues, RetainedComputedCursorList, RetainedComputedFilterOperationList,
    RetainedComputedResolvedTransformList, RetainedComputedShadowList, RetainedComputedSvgDashList,
    RetainedGridAreaList, RetainedGridNameIndexList, RetainedGridTrackEntryList, RetainedPositionAreaList,
    RetainedPositionTryFallbackList, TransformValues,
};
use crate::css::computed_values::{
    FfiGroupValueEntry, GROUP_FIELD_COLOR, GROUP_FIELD_COLOR_OR_KEYWORD, GROUP_FIELD_RESOLVED_F32,
    GROUP_FIELD_RESOLVED_F64, GROUP_FIELD_RESOLVED_U8, registered_group_field_descriptors, rust_build_alignment_group,
    rust_build_grid_group, rust_build_inherited_box_group, rust_build_inherited_table_group, rust_build_sizing_group,
    rust_build_style_group, rust_build_surround_group, rust_build_svg_reset_group, rust_build_text_reset_group,
};
use crate::css::css_enums::keyword;
use crate::css::css_pixels::CssPixels;
use crate::css::property_metadata::property_id;
use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};
use crate::css::style_value::{GridTrackEntryKind, RetainedGridTrackEntry, StyleValueData};

/// Mirror of the C++ StyleGroupIndex numbering; ComputedValues.cpp
/// static-asserts these values against the enum, and the entry point asserts
/// the caller's group count.
mod group_index {
    pub const INHERITED_TABLE: usize = 0;
    pub const INHERITED_LIST: usize = 1;
    pub const INHERITED_UI: usize = 2;
    pub const INHERITED_SVG: usize = 3;
    pub const INHERITED_TEXT: usize = 4;
    pub const INHERITED_BOX: usize = 5;
    pub const FONT: usize = 6;
    pub const ANIMATION: usize = 7;
    pub const SVG_RESET: usize = 8;
    pub const GRID: usize = 9;
    pub const ANCHOR: usize = 10;
    pub const EFFECTS: usize = 11;
    pub const MASK: usize = 12;
    pub const TEXT_RESET: usize = 13;
    pub const CONTENT: usize = 14;
    pub const TRANSFORM: usize = 15;
    pub const BACKGROUND: usize = 16;
    pub const BORDER: usize = 17;
    pub const ALIGNMENT: usize = 18;
    pub const MISC_RESET: usize = 19;
    pub const SIZING: usize = 20;
    pub const SURROUND: usize = 21;
    pub const BOX: usize = 22;
    pub const COUNT: usize = 23;
}

/// The pre-resolved inputs one table-driven group build needs from C++: the
/// color resolution context (whose current color is the element's own
/// resolved color), the used color-scheme, and the sparse effective-value
/// overrides.
#[repr(C)]
pub struct FfiTableGroupBuildInputs {
    /// A marshalled StyleValueFFI::FfiColorResolutionInput whose current
    /// color is the element's own resolved color, matching the context the
    /// C++ sidecar loops resolved against.
    pub color_input: *const c_void,
    /// The used color-scheme code (PreferredColorScheme underlying value).
    pub used_color_scheme: u8,
    /// Longhands whose effective computed value differs from the table slot -
    /// the animated overlay and partial-drive specified-value preferences -
    /// as parallel property-id and value-data spans.
    pub override_properties: *const u16,
    pub override_values: *const *const c_void,
    pub override_count: usize,
    /// The raw bits of the C++ Display value before the box type
    /// transformation, a C++-side member the table does not hold.
    pub box_display_before_transformation_raw: u32,
    pub font: *const FfiFontGroupBuildInputs,
}

/// Platform font resources and derived facts supplied to the Rust-owned font
/// group builder. The pointers borrow objects pinned by the document's font
/// computer.
#[repr(C)]
pub struct FfiFontGroupBuildInputs {
    pub font_size_raw: i32,
    pub line_height_used_raw: i32,
    pub font_variant_emoji: u8,
    pub font_ascent: f32,
    pub font_descent: f32,
    pub font_x_height: f32,
    pub first_available_font: *const c_void,
    pub font_cascade_list: *const c_void,
    pub font_weight: f64,
    pub font_width: f64,
    pub math_shift: u8,
    pub math_style: u8,
    pub math_depth: i32,
}

/// The longhand table joined with the effective-value overrides: exactly the
/// values `ComputedStyleWorkingSet::property()` returns during a group build.
struct EffectiveValues<'a> {
    table: &'a ComputedLonghandTable,
    override_properties: &'a [u16],
    override_values: &'a [*const c_void],
}

const MAX_GROUP_FIELD_COUNT: usize = 32;

struct GroupValueEntries {
    entries: [std::mem::MaybeUninit<FfiGroupValueEntry>; MAX_GROUP_FIELD_COUNT],
    len: usize,
}

impl GroupValueEntries {
    fn new() -> Self {
        Self {
            entries: [const { std::mem::MaybeUninit::uninit() }; MAX_GROUP_FIELD_COUNT],
            len: 0,
        }
    }

    fn push(&mut self, entry: FfiGroupValueEntry) {
        assert!(
            self.len < self.entries.len(),
            "a computed style group has too many fields"
        );
        self.entries[self.len].write(entry);
        self.len += 1;
    }
}

impl std::ops::Deref for GroupValueEntries {
    type Target = [FfiGroupValueEntry];

    fn deref(&self) -> &Self::Target {
        // SAFETY: push initializes every entry below len and FfiGroupValueEntry has no drop glue.
        unsafe { std::slice::from_raw_parts(self.entries.as_ptr().cast(), self.len) }
    }
}

impl EffectiveValues<'_> {
    fn pointer(&self, property_id: u16) -> *const c_void {
        if let Some(index) = self.override_properties.iter().position(|id| *id == property_id) {
            return self.override_values[index];
        }
        match self.table.get(property_id) {
            Some(value) => value.pointer().cast(),
            None => std::ptr::null(),
        }
    }

    fn value(&self, property_id: u16) -> Option<&StyleValueData> {
        // SAFETY: Table slots retain their data and override pointers borrow
        // values the caller keeps alive across the build.
        unsafe { self.pointer(property_id).cast::<StyleValueData>().as_ref() }
    }
}

/// The C++ Color::value() bit layout.
fn packed_color(color: Rgba) -> u32 {
    (u32::from(color.a) << 24) | (u32::from(color.r) << 16) | (u32::from(color.g) << 8) | u32::from(color.b)
}

/// The resolved color the C++ sidecar loops produced for one color-kind
/// field, or None when the value needs the C++ path.
fn resolved_color(input: &ColorResolutionInput, property: u16, data: &StyleValueData) -> Option<u32> {
    // The element's own color is resolved by the caller against the incoming
    // context (whose currentcolor is the parent's), not re-resolved here.
    if property == property_id::COLOR {
        return input.current_color.map(packed_color);
    }
    if let StyleValueData::Keyword { keyword: code } = data {
        // An auto caret-color is the element's own color; an auto
        // accent-color is the system accent color for the used scheme.
        if property == property_id::CARET_COLOR && *code == keyword::AUTO {
            return input.current_color.map(packed_color);
        }
        if property == property_id::ACCENT_COLOR && *code == keyword::AUTO {
            return Some(packed_color(accent_color(
                input.scheme == Some(PREFERRED_COLOR_SCHEME_DARK),
            )));
        }
    }
    to_color(data, input).map(packed_color)
}

/// The resolved number for opacity-normalized and superellipse fields, whose
/// computed values wrap an absolutized plain number - or, for superellipse
/// corner shapes, a calculation resolvable without context, matching the C++
/// number_from_style_value the population path used.
fn resolved_wrapped_number(data: &StyleValueData) -> Option<f64> {
    let inner = match data {
        StyleValueData::OpacityValue { value } => value.data(),
        StyleValueData::Superellipse { parameter } => parameter.data(),
        _ => return None,
    };
    match inner {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_number_without_context(inner),
        _ => None,
    }
}

/// Gathers one group's descriptor entries from the table, resolving the
/// color and number sidecars the descriptor kinds ask for.
unsafe fn gather_group_entries(
    group_index: usize,
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
) -> Option<GroupValueEntries> {
    let descriptors = registered_group_field_descriptors(group_index)?;
    assert!(descriptors.len() <= MAX_GROUP_FIELD_COUNT);
    let mut entries = GroupValueEntries::new();
    for descriptor in descriptors {
        let data_pointer = values.pointer(descriptor.property_id);
        let data = unsafe { data_pointer.cast::<StyleValueData>().as_ref() }?;
        let mut entry = FfiGroupValueEntry {
            data: data_pointer,
            resolved_color: 0,
            has_resolved_color: false,
            resolved_number: 0.0,
            has_resolved_number: false,
        };
        match descriptor.kind {
            GROUP_FIELD_COLOR | GROUP_FIELD_COLOR_OR_KEYWORD => {
                if let Some(color) = resolved_color(input, descriptor.property_id, data) {
                    entry.resolved_color = color;
                    entry.has_resolved_color = true;
                }
            }
            GROUP_FIELD_RESOLVED_F32 | GROUP_FIELD_RESOLVED_F64 => {
                if let Some(number) = resolved_wrapped_number(data) {
                    entry.resolved_number = number;
                    entry.has_resolved_number = true;
                }
            }
            GROUP_FIELD_RESOLVED_U8 => {
                // The only resolved-u8 field is the used color-scheme.
                entry.resolved_number = f64::from(used_color_scheme);
                entry.has_resolved_number = true;
            }
            _ => {}
        }
        entries.push(entry);
    }
    Some(entries)
}

/// Builds one descriptor-driven group through `rust_build_style_group`,
/// gathering the entries from the table instead of a marshalled span.
unsafe fn build_generic_group(
    group_index: usize,
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index, values, input, used_color_scheme) }) else {
        return std::ptr::null();
    };
    // SAFETY: The entries hold live value data gathered above and the caller
    // warrants the parent payload.
    unsafe { rust_build_style_group(group_index, entries.as_ptr(), entries.len(), parent_payload) }
}

unsafe fn build_surround_group(values: &EffectiveValues, parent_payload: *const c_void) -> *const c_void {
    let position_anchor = match values.value(property_id::POSITION_ANCHOR) {
        Some(StyleValueData::CustomIdent { .. }) => values.pointer(property_id::POSITION_ANCHOR),
        _ => std::ptr::null(),
    };
    // SAFETY: Every pointer names live value data from the table and the
    // caller warrants the parent payload.
    unsafe {
        rust_build_surround_group(
            group_index::SURROUND,
            values.pointer(property_id::TOP),
            values.pointer(property_id::RIGHT),
            values.pointer(property_id::BOTTOM),
            values.pointer(property_id::LEFT),
            values.pointer(property_id::MARGIN_TOP),
            values.pointer(property_id::MARGIN_RIGHT),
            values.pointer(property_id::MARGIN_BOTTOM),
            values.pointer(property_id::MARGIN_LEFT),
            values.pointer(property_id::PADDING_TOP),
            values.pointer(property_id::PADDING_RIGHT),
            values.pointer(property_id::PADDING_BOTTOM),
            values.pointer(property_id::PADDING_LEFT),
            position_anchor,
            parent_payload,
        )
    }
}

unsafe fn build_alignment_group(values: &EffectiveValues, parent_payload: *const c_void) -> *const c_void {
    let number = |property: u16| required_number(values.value(property).expect("the table holds the flex factor"));
    let flex_grow = number(property_id::FLEX_GROW);
    let flex_shrink = number(property_id::FLEX_SHRINK);
    let order = required_integer(values.value(property_id::ORDER).expect("the table holds order"));
    // SAFETY: Every pointer names live value data from the table and the
    // caller warrants the parent payload.
    unsafe {
        rust_build_alignment_group(
            group_index::ALIGNMENT,
            values.pointer(property_id::FLEX_DIRECTION),
            values.pointer(property_id::FLEX_WRAP),
            values.pointer(property_id::FLEX_BASIS),
            flex_grow,
            flex_shrink,
            order,
            values.pointer(property_id::ALIGN_CONTENT),
            values.pointer(property_id::ALIGN_ITEMS),
            values.pointer(property_id::ALIGN_SELF),
            values.pointer(property_id::JUSTIFY_CONTENT),
            values.pointer(property_id::JUSTIFY_ITEMS),
            values.pointer(property_id::JUSTIFY_SELF),
            values.pointer(property_id::COLUMN_GAP),
            values.pointer(property_id::ROW_GAP),
            parent_payload,
        )
    }
}

unsafe fn build_svg_reset_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    parent_payload: *const c_void,
) -> *const c_void {
    let color = |property: u16| Some(packed_color(to_color(values.value(property)?, input)?));
    let opacity = |property: u16| resolved_wrapped_number(values.value(property)?);
    let (Some(stop_color), Some(stop_opacity), Some(flood_color), Some(flood_opacity)) = (
        color(property_id::STOP_COLOR),
        opacity(property_id::STOP_OPACITY),
        color(property_id::FLOOD_COLOR),
        opacity(property_id::FLOOD_OPACITY),
    ) else {
        return std::ptr::null();
    };
    // SAFETY: Every pointer names live value data from the table and the
    // caller warrants the parent payload.
    unsafe {
        rust_build_svg_reset_group(
            group_index::SVG_RESET,
            values.pointer(property_id::CX),
            values.pointer(property_id::CY),
            values.pointer(property_id::D),
            values.pointer(property_id::R),
            values.pointer(property_id::RX),
            values.pointer(property_id::RY),
            values.pointer(property_id::X),
            values.pointer(property_id::Y),
            stop_color,
            stop_opacity as f32,
            flood_color,
            flood_opacity as f32,
            values.pointer(property_id::VECTOR_EFFECT),
            parent_payload,
        )
    }
}

/// The name table, name-index spans, track entries and named areas one grid
/// group build accumulates: the Rust twin of the GridGroupBuilderArena the
/// C++ marshalled build used, interning by raw fly-string word since fly
/// strings are interned.
struct GridGroupArena {
    names: Vec<RetainedUtf16FlyString>,
    indices_by_raw: HashMap<usize, u32>,
    name_indices: Vec<u32>,
    entries: Vec<ComputedGridTrackEntry>,
    areas: Vec<ComputedGridArea>,
}

impl GridGroupArena {
    fn new() -> Self {
        Self {
            names: Vec::new(),
            indices_by_raw: HashMap::new(),
            name_indices: Vec::new(),
            entries: Vec::new(),
            areas: Vec::new(),
        }
    }

    /// Interns one name, taking ownership of the passed reference; a
    /// duplicate drops its surplus reference on return.
    fn intern_retained(&mut self, name: RetainedUtf16FlyString) -> u32 {
        let raw = name.raw();
        assert_ne!(raw, 0, "cannot intern the no-name sentinel");
        if let Some(index) = self.indices_by_raw.get(&raw) {
            return *index;
        }
        let index = self.names.len() as u32;
        self.names.push(name);
        self.indices_by_raw.insert(raw, index);
        index
    }

    fn intern_borrowed(&mut self, name: &RetainedUtf16FlyString) -> u32 {
        self.intern_retained(name.clone())
    }
}

fn auto_grid_track_breadth() -> ComputedGridTrackBreadth {
    ComputedGridTrackBreadth {
        is_flex: false,
        flex_factor: 0.0,
        size: ComputedSize::keyword(ComputedSizeKind::Auto),
    }
}

/// One track breadth from its computed grid-size value, with the C++
/// GridSize lowering: flex values - including calc that resolves to flex -
/// carry their fr factor, everything else maps through the shared computed
/// size representation.
fn grid_track_breadth(data: &StyleValueData) -> ComputedGridTrackBreadth {
    let flex_factor = match data {
        StyleValueData::Flex { value, unit } => {
            Some(value * crate::css::calc::FLEX_UNIT_CANONICAL_RATIOS[*unit as usize])
        }
        StyleValueData::Calculated { .. } => resolve_calculated_flex_without_context(data),
        _ => None,
    };
    if let Some(flex_factor) = flex_factor {
        return ComputedGridTrackBreadth {
            is_flex: true,
            flex_factor,
            size: ComputedSize::keyword(ComputedSizeKind::Auto),
        };
    }
    let size = ComputedSize::from_data(std::ptr::from_ref(data).cast());
    assert!(size.kind != ComputedSizeKind::None, "a grid size cannot be none");
    ComputedGridTrackBreadth {
        is_flex: false,
        flex_factor: 0.0,
        size,
    }
}

/// The C++ int_from_style_value: a plain integer, or a calculation that must
/// resolve to one with no external context.
fn grid_integer(data: &StyleValueData) -> Option<i32> {
    match data {
        StyleValueData::Integer { value } => Some(*value),
        StyleValueData::Calculated { .. } => resolve_calculated_integer_without_context(data),
        _ => None,
    }
}

fn build_grid_track_list(
    is_subgrid: bool,
    preserves_line_name_sets: bool,
    source_entries: &[RetainedGridTrackEntry],
    arena: &mut GridGroupArena,
) -> Option<ComputedGridTrackList> {
    let mut result = ComputedGridTrackList {
        is_subgrid,
        preserves_line_name_sets,
        first_entry: GRID_NO_INDEX,
    };
    let mut previous_entry = GRID_NO_INDEX;

    for source in source_entries {
        let entry_index = arena.entries.len() as u32;
        arena.entries.push(ComputedGridTrackEntry {
            kind: ComputedGridTrackEntryKind::LineNames as u8,
            next_sibling: GRID_NO_INDEX,
            name_index_start: 0,
            name_index_count: 0,
            size: auto_grid_track_breadth(),
            min_size: auto_grid_track_breadth(),
            max_size: auto_grid_track_breadth(),
            repeat_type: 0,
            repeat_count: 0,
            repeat_list: ComputedGridTrackList {
                is_subgrid: false,
                preserves_line_name_sets: false,
                first_entry: GRID_NO_INDEX,
            },
        });

        if result.first_entry == GRID_NO_INDEX {
            result.first_entry = entry_index;
        }
        if previous_entry != GRID_NO_INDEX {
            arena.entries[previous_entry as usize].next_sibling = entry_index;
        }
        previous_entry = entry_index;

        match source.kind {
            GridTrackEntryKind::LineNames => {
                let name_index_start = arena.name_indices.len();
                for name in source.names.as_slice() {
                    let index = arena.intern_borrowed(name);
                    arena.name_indices.push(index);
                }
                let entry = &mut arena.entries[entry_index as usize];
                entry.name_index_start = name_index_start;
                entry.name_index_count = arena.name_indices.len() - name_index_start;
            }
            GridTrackEntryKind::Size => {
                let size = grid_track_breadth(source.size_value.data());
                let entry = &mut arena.entries[entry_index as usize];
                entry.kind = ComputedGridTrackEntryKind::TrackSize as u8;
                entry.size = size;
            }
            GridTrackEntryKind::MinMax => {
                let min_size = grid_track_breadth(source.min_value.data());
                let max_size = grid_track_breadth(source.max_value.data());
                let entry = &mut arena.entries[entry_index as usize];
                entry.kind = ComputedGridTrackEntryKind::MinMax as u8;
                entry.min_size = min_size;
                entry.max_size = max_size;
            }
            GridTrackEntryKind::Repeat => {
                // The repeat-type codes are pinned by static asserts beside
                // the C++ GridRepeatType enum: auto-fit 0, auto-fill 1,
                // fixed 2. Only a fixed repeat carries a count.
                const GRID_REPEAT_TYPE_FIXED: u8 = 2;
                let repeat_list = build_grid_track_list(
                    source.repeat_is_subgrid,
                    source.repeat_preserve_line_name_sets,
                    source.repeat_entries(),
                    arena,
                )?;
                let repeat_count = if source.repeat_type == GRID_REPEAT_TYPE_FIXED {
                    grid_integer(source.repeat_count.data())? as usize
                } else {
                    0
                };
                let entry = &mut arena.entries[entry_index as usize];
                entry.kind = ComputedGridTrackEntryKind::Repeat as u8;
                entry.repeat_type = source.repeat_type;
                entry.repeat_count = repeat_count;
                entry.repeat_list = repeat_list;
            }
        }
    }
    Some(result)
}

fn build_grid_placement(data: &StyleValueData, arena: &mut GridGroupArena) -> Option<ComputedGridPlacement> {
    let StyleValueData::GridTrackPlacement {
        kind,
        value,
        has_name,
        name,
        implicit_start_name,
        implicit_end_name,
    } = data
    else {
        return None;
    };
    let mut result = ComputedGridPlacement {
        kind: ComputedGridPlacementKind::Auto as u8,
        has_line_number: false,
        line_number: 0,
        has_name: false,
        name_index: GRID_NO_INDEX,
        implicit_start_name_index: GRID_NO_INDEX,
        implicit_end_name_index: GRID_NO_INDEX,
    };
    match kind {
        0 => {}
        1 => {
            result.kind = ComputedGridPlacementKind::Span as u8;
            result.has_line_number = true;
            result.line_number = grid_integer(value.optional_data()?)?;
            if *has_name {
                result.has_name = true;
                result.name_index = arena.intern_borrowed(name);
            }
        }
        _ => {
            result.kind = ComputedGridPlacementKind::Line as u8;
            if let Some(line_number) = value.optional_data() {
                result.has_line_number = true;
                result.line_number = grid_integer(line_number)?;
            }
            if *has_name {
                result.has_name = true;
                result.name_index = arena.intern_borrowed(name);
                result.implicit_start_name_index = arena.intern_borrowed(implicit_start_name);
                result.implicit_end_name_index = arena.intern_borrowed(implicit_end_name);
            }
        }
    }
    Some(result)
}

/// Builds the grid group payload from the table's computed track lists,
/// template areas and placements, in the C++ marshalled build's interning
/// order so the arena layout stays deterministic.
unsafe fn build_grid_group(values: &EffectiveValues, parent_payload: *const c_void) -> *const c_void {
    // An all-initial grid shares the immortal default payload directly, with
    // its empty style value handles, exactly as the descriptor constraints
    // used to.
    const GRID_PROPERTIES: [u16; 9] = [
        property_id::GRID_TEMPLATE_COLUMNS,
        property_id::GRID_TEMPLATE_ROWS,
        property_id::GRID_AUTO_COLUMNS,
        property_id::GRID_AUTO_ROWS,
        property_id::GRID_TEMPLATE_AREAS,
        property_id::GRID_COLUMN_START,
        property_id::GRID_COLUMN_END,
        property_id::GRID_ROW_START,
        property_id::GRID_ROW_END,
    ];
    let all_initial = GRID_PROPERTIES
        .iter()
        .all(|property| values.pointer(*property) == crate::css::style_compute::initial_value_data(*property).cast());
    if all_initial {
        // SAFETY: The caller warrants the parent payload.
        return unsafe { crate::css::computed_values::share_default_group_payload(group_index::GRID, parent_payload) };
    }

    let mut arena = GridGroupArena::new();

    let track_list = |arena: &mut GridGroupArena, property: u16| -> Option<ComputedGridTrackList> {
        match values.value(property)? {
            StyleValueData::GridTrackSizeList {
                is_subgrid,
                preserve_line_name_sets,
                entries,
            } => build_grid_track_list(*is_subgrid, *preserve_line_name_sets, entries.as_slice(), arena),
            _ => None,
        }
    };
    let Some(template_columns) = track_list(&mut arena, property_id::GRID_TEMPLATE_COLUMNS) else {
        return std::ptr::null();
    };
    let Some(template_rows) = track_list(&mut arena, property_id::GRID_TEMPLATE_ROWS) else {
        return std::ptr::null();
    };
    let Some(auto_columns) = track_list(&mut arena, property_id::GRID_AUTO_COLUMNS) else {
        return std::ptr::null();
    };
    let Some(auto_rows) = track_list(&mut arena, property_id::GRID_AUTO_ROWS) else {
        return std::ptr::null();
    };

    let Some(StyleValueData::GridTemplateArea { grid_areas, .. }) = values.value(property_id::GRID_TEMPLATE_AREAS)
    else {
        return std::ptr::null();
    };
    for area in grid_areas.as_slice() {
        let [row_start, row_end, column_start, column_end] = area.grid_lines();
        let name_index = arena.intern_borrowed(area.name());
        let implicit_start_name_index = arena.intern_borrowed(area.implicit_start_name());
        let implicit_end_name_index = arena.intern_borrowed(area.implicit_end_name());
        arena.areas.push(ComputedGridArea {
            name_index,
            implicit_start_name_index,
            implicit_end_name_index,
            row_start,
            row_end,
            column_start,
            column_end,
        });
    }

    let placement = |arena: &mut GridGroupArena, property: u16| -> Option<ComputedGridPlacement> {
        build_grid_placement(values.value(property)?, arena)
    };
    let Some(column_start) = placement(&mut arena, property_id::GRID_COLUMN_START) else {
        return std::ptr::null();
    };
    let Some(column_end) = placement(&mut arena, property_id::GRID_COLUMN_END) else {
        return std::ptr::null();
    };
    let Some(row_start) = placement(&mut arena, property_id::GRID_ROW_START) else {
        return std::ptr::null();
    };
    let Some(row_end) = placement(&mut arena, property_id::GRID_ROW_END) else {
        return std::ptr::null();
    };

    let retained_slot = |property: u16| {
        // SAFETY: Table slots and override values are live style value data
        // across the build.
        ComputedStyleValueHandle::retained(values.pointer(property).cast())
    };
    let built = GridValues {
        names: RetainedUtf16FlyStringList::from_retained_strings(arena.names),
        name_indices: RetainedGridNameIndexList::from_vec(arena.name_indices),
        entries: RetainedGridTrackEntryList::from_vec(arena.entries),
        areas: RetainedGridAreaList::from_vec(arena.areas),
        template_columns,
        template_rows,
        auto_columns,
        auto_rows,
        column_start,
        column_end,
        row_start,
        row_end,
        grid_template_columns_style_value: retained_slot(property_id::GRID_TEMPLATE_COLUMNS),
        grid_template_rows_style_value: retained_slot(property_id::GRID_TEMPLATE_ROWS),
        grid_auto_columns_style_value: retained_slot(property_id::GRID_AUTO_COLUMNS),
        grid_auto_rows_style_value: retained_slot(property_id::GRID_AUTO_ROWS),
        grid_template_areas_style_value: retained_slot(property_id::GRID_TEMPLATE_AREAS),
        grid_column_start_style_value: retained_slot(property_id::GRID_COLUMN_START),
        grid_column_end_style_value: retained_slot(property_id::GRID_COLUMN_END),
        grid_row_start_style_value: retained_slot(property_id::GRID_ROW_START),
        grid_row_end_style_value: retained_slot(property_id::GRID_ROW_END),
    };
    // SAFETY: The built payload is fully initialized; the builder assumes
    // ownership of it, so the local value must not drop.
    let payload = unsafe { rust_build_grid_group(group_index::GRID, &raw const built, parent_payload) };
    std::mem::forget(built);
    payload
}

// --- Transform and effects lowering ---------------------------------------
//
// The paint-ready forms the C++ fallback used to build from wrapper reads -
// baked matrices, resolved filter operations, shadow data - are computed here
// from the table slots. The members that genuinely need C++ (style value
// wrappers, LengthPercentage slots for reference-box-dependent percentages)
// travel as retained handles in an assembly struct that a registered C++
// assembler transcribes into the payload.

/// The C++ TransformFunctionParameterType codes the generated parameter
/// table uses.
const TRANSFORM_PARAMETER_ANGLE: u8 = 0;
const TRANSFORM_PARAMETER_LENGTH: u8 = 1;
const TRANSFORM_PARAMETER_LENGTH_NONE: u8 = 2;
const TRANSFORM_PARAMETER_LENGTH_PERCENTAGE: u8 = 3;
const TRANSFORM_PARAMETER_NUMBER: u8 = 4;
const TRANSFORM_PARAMETER_NUMBER_PERCENTAGE: u8 = 5;

fn angle_unit_index(name: &str) -> usize {
    crate::css::calc::ANGLE_UNIT_NAMES
        .iter()
        .position(|unit| *unit == name)
        .expect("the angle unit table names every unit")
}

/// The C++ Angle::to_radians: the ratio between the value's unit and rad,
/// times the value.
fn angle_value_to_radians(value: f64, unit: u8) -> f64 {
    let rad = angle_unit_index("rad");
    if unit as usize == rad {
        return value;
    }
    (crate::css::calc::ANGLE_UNIT_CANONICAL_RATIOS[unit as usize] / crate::css::calc::ANGLE_UNIT_CANONICAL_RATIOS[rad])
        * value
}

/// An angle-typed transform argument in radians, with the C++ conversion
/// order: calc resolves to canonical degrees first.
fn transform_angle_radians(data: &StyleValueData) -> f64 {
    match data {
        StyleValueData::Angle { value, unit } => angle_value_to_radians(*value, *unit),
        StyleValueData::Calculated { .. } => {
            let degrees = crate::css::calc::resolve_calculated_angle_without_context(data)
                .expect("a computed transform angle resolves without context");
            let deg = crate::css::calc::ANGLE_UNIT_CANONICAL_RATIOS
                .iter()
                .position(|ratio| *ratio == 1.0)
                .expect("the angle unit table has a canonical unit");
            angle_value_to_radians(degrees, deg as u8)
        }
        _ => unreachable!("a computed transform angle is an angle or a calculation"),
    }
}

/// An angle in degrees, for hue-rotate.
fn angle_degrees(data: &StyleValueData) -> f64 {
    match data {
        StyleValueData::Angle { value, unit } => crate::css::calc::ANGLE_UNIT_CANONICAL_RATIOS[*unit as usize] * value,
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_angle_without_context(data)
            .expect("a computed filter angle resolves without context"),
        _ => unreachable!("a computed angle value is an angle or a calculation"),
    }
}

/// The C++ Length::absolute_length_to_px_without_rounding for a length value,
/// with calc resolving to px with no external context.
fn length_to_px_unrounded(data: &StyleValueData) -> f64 {
    match data {
        StyleValueData::Length { value, unit } => {
            let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
            assert!(ratio.is_finite(), "computed length is not absolute");
            ratio * value
        }
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_length_without_context(data, 0.0)
            .expect("a computed length resolves without context"),
        _ => unreachable!("a computed length value is a length or a calculation"),
    }
}

/// The C++ Length::absolute_length_to_px: the unrounded pixels quantized to
/// CSSPixels.
fn length_to_css_pixels(data: &StyleValueData) -> CssPixels {
    CssPixels::nearest_value_for(length_to_px_unrounded(data))
}

/// The C++ number_from_style_value with a percentage basis of one.
fn transform_number(data: &StyleValueData) -> f64 {
    match data {
        StyleValueData::Number { value } => *value,
        StyleValueData::Percentage { value } => value * 0.01,
        StyleValueData::Calculated { .. } => {
            if let Some(number) = crate::css::calc::resolve_calculated_number_without_context(data) {
                return number;
            }
            crate::css::calc::resolve_calculated_percentage_without_context(data)
                .expect("a computed transform number resolves without context")
                * 0.01
        }
        _ => unreachable!("a computed transform number is a number, percentage or calculation"),
    }
}

fn value_contains_percentage(data: &StyleValueData) -> bool {
    match data {
        StyleValueData::Percentage { .. } => true,
        StyleValueData::Calculated { .. } => {
            // SAFETY: The calculated style value outlives the query.
            unsafe {
                let root = crate::css::calc::rust_calc_root_from_calculated(std::ptr::from_ref(data).cast());
                assert!(!root.is_null());
                crate::css::calc::rust_calc_node_contains_percentage(root)
            }
        }
        _ => false,
    }
}

fn matrix_identity() -> [f32; 16] {
    [
        1.0, 0.0, 0.0, 0.0, //
        0.0, 1.0, 0.0, 0.0, //
        0.0, 0.0, 1.0, 0.0, //
        0.0, 0.0, 0.0, 1.0,
    ]
}

fn matrix_translation(x: f32, y: f32, z: f32) -> [f32; 16] {
    [
        1.0, 0.0, 0.0, x, //
        0.0, 1.0, 0.0, y, //
        0.0, 0.0, 1.0, z, //
        0.0, 0.0, 0.0, 1.0,
    ]
}

fn matrix_scale(x: f32, y: f32, z: f32) -> [f32; 16] {
    [
        x, 0.0, 0.0, 0.0, //
        0.0, y, 0.0, 0.0, //
        0.0, 0.0, z, 0.0, //
        0.0, 0.0, 0.0, 1.0,
    ]
}

fn matrix_perspective(distance: f32) -> [f32; 16] {
    [
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        -1.0 / distance,
        1.0,
    ]
}

/// The Gfx rotation_matrix port, with its epsilon clamps on the sine and
/// cosine.
fn matrix_rotation(axis: [f32; 3], angle: f32) -> [f32; 16] {
    let mut s = angle.sin();
    let mut c = angle.cos();
    if c.abs() < f32::EPSILON {
        c = 0.0;
    }
    if s.abs() < f32::EPSILON {
        s = 0.0;
    }
    let t = 1.0 - c;
    let [x, y, z] = axis;
    [
        t * x * x + c,
        t * x * y - z * s,
        t * x * z + y * s,
        0.0,
        t * x * y + z * s,
        t * y * y + c,
        t * y * z - x * s,
        0.0,
        t * x * z - y * s,
        t * y * z + x * s,
        t * z * z + c,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    ]
}

/// The TransformationStyleValue::to_matrix port for computed values, which
/// never carry reference-box percentages here: the translate family's
/// percentage-bearing values lower into per-axis slots instead.
fn transformation_to_matrix(function: u8, values: &[crate::css::style_value::RetainedStyleValueData]) -> [f32; 16] {
    use crate::css::serialize::transform_function as functions;

    let parameters = crate::css::serialize::TRANSFORM_FUNCTION_PARAMETER_TYPES[function as usize];
    let count = values.len();
    let get_value = |index: usize| -> f32 {
        let data = values[index].data();
        match parameters[index] {
            TRANSFORM_PARAMETER_ANGLE => transform_angle_radians(data) as f32,
            TRANSFORM_PARAMETER_LENGTH | TRANSFORM_PARAMETER_LENGTH_NONE | TRANSFORM_PARAMETER_LENGTH_PERCENTAGE => {
                length_to_css_pixels(data).to_float()
            }
            TRANSFORM_PARAMETER_NUMBER | TRANSFORM_PARAMETER_NUMBER_PERCENTAGE => transform_number(data) as f32,
            _ => unreachable!("the parameter table holds only known parameter types"),
        }
    };

    match function {
        functions::PERSPECTIVE => {
            // https://drafts.csswg.org/css-transforms-2/#perspective
            if count == 1 {
                if matches!(values[0].data(), StyleValueData::Keyword { keyword: code } if *code == keyword::NONE) {
                    return matrix_identity();
                }
                // A depth below 1px acts as 1px for rendering and for the
                // resolved transform value.
                let distance = get_value(0);
                return matrix_perspective(distance.max(1.0));
            }
        }
        functions::MATRIX => {
            if count == 6 {
                let m: Vec<f32> = (0..6).map(get_value).collect();
                return [
                    m[0], m[2], 0.0, m[4], //
                    m[1], m[3], 0.0, m[5], //
                    0.0, 0.0, 1.0, 0.0, //
                    0.0, 0.0, 0.0, 1.0,
                ];
            }
        }
        functions::MATRIX3D => {
            if count == 16 {
                let m: Vec<f32> = (0..16).map(get_value).collect();
                return [
                    m[0], m[4], m[8], m[12], //
                    m[1], m[5], m[9], m[13], //
                    m[2], m[6], m[10], m[14], //
                    m[3], m[7], m[11], m[15],
                ];
            }
        }
        functions::TRANSLATE => {
            if count == 1 {
                return matrix_translation(get_value(0), 0.0, 0.0);
            }
            if count == 2 {
                return matrix_translation(get_value(0), get_value(1), 0.0);
            }
        }
        functions::TRANSLATE3D => {
            if count == 3 {
                return matrix_translation(get_value(0), get_value(1), get_value(2));
            }
        }
        functions::TRANSLATE_X => {
            if count == 1 {
                return matrix_translation(get_value(0), 0.0, 0.0);
            }
        }
        functions::TRANSLATE_Y => {
            if count == 1 {
                return matrix_translation(0.0, get_value(0), 0.0);
            }
        }
        functions::TRANSLATE_Z => {
            if count == 1 {
                return matrix_translation(0.0, 0.0, get_value(0));
            }
        }
        functions::SCALE => {
            if count == 1 {
                let scale = get_value(0);
                return matrix_scale(scale, scale, 1.0);
            }
            if count == 2 {
                return matrix_scale(get_value(0), get_value(1), 1.0);
            }
        }
        functions::SCALE3D => {
            if count == 3 {
                return matrix_scale(get_value(0), get_value(1), get_value(2));
            }
        }
        functions::SCALE_X => {
            if count == 1 {
                return matrix_scale(get_value(0), 1.0, 1.0);
            }
        }
        functions::SCALE_Y => {
            if count == 1 {
                return matrix_scale(1.0, get_value(0), 1.0);
            }
        }
        functions::SCALE_Z => {
            if count == 1 {
                return matrix_scale(1.0, 1.0, get_value(0));
            }
        }
        functions::ROTATE3D => {
            if count == 4 {
                let axis = [get_value(0), get_value(1), get_value(2)];
                // The Gfx VectorN dot accumulates in component order.
                let length_squared = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
                let length = length_squared.sqrt();
                let epsilon = 1e-5f32;
                if length < epsilon {
                    return matrix_identity();
                }
                let inverse_length = 1.0 / length;
                let normalized = [
                    axis[0] * inverse_length,
                    axis[1] * inverse_length,
                    axis[2] * inverse_length,
                ];
                return matrix_rotation(normalized, get_value(3));
            }
        }
        functions::ROTATE_X => {
            if count == 1 {
                return matrix_rotation([1.0, 0.0, 0.0], get_value(0));
            }
        }
        functions::ROTATE_Y => {
            if count == 1 {
                return matrix_rotation([0.0, 1.0, 0.0], get_value(0));
            }
        }
        functions::ROTATE | functions::ROTATE_Z => {
            if count == 1 {
                return matrix_rotation([0.0, 0.0, 1.0], get_value(0));
            }
        }
        functions::SKEW => {
            if count == 1 {
                let mut matrix = matrix_identity();
                matrix[1] = get_value(0).tan();
                return matrix;
            }
            if count == 2 {
                let mut matrix = matrix_identity();
                matrix[1] = get_value(0).tan();
                matrix[4] = get_value(1).tan();
                return matrix;
            }
        }
        functions::SKEW_X => {
            if count == 1 {
                let mut matrix = matrix_identity();
                matrix[1] = get_value(0).tan();
                return matrix;
            }
        }
        functions::SKEW_Y if count == 1 => {
            let mut matrix = matrix_identity();
            matrix[4] = get_value(0).tan();
            return matrix;
        }
        _ => {}
    }
    // The C++ to_matrix logs and falls back to the identity for unhandled
    // function and argument-count combinations.
    matrix_identity()
}

fn baked_matrix_entry(matrix: [f32; 16]) -> ComputedResolvedTransform {
    ComputedResolvedTransform {
        is_translate: false,
        matrix,
        x_px: 0.0,
        y_px: 0.0,
        z_px: 0.0,
        x_percentage: ComputedStyleValueHandle::empty(),
        y_percentage: ComputedStyleValueHandle::empty(),
    }
}

/// The TransformationStyleValue::to_resolved_transform port: only the
/// translate family takes length-percentages, so a percentage-bearing
/// translate keeps per-axis slots and everything else bakes into a matrix.
fn lower_transformation(data: &StyleValueData) -> ComputedResolvedTransform {
    use crate::css::serialize::transform_function as functions;

    let StyleValueData::Transformation {
        transform_function,
        values,
        ..
    } = data
    else {
        unreachable!("a computed transform function is a transformation value");
    };
    let function = *transform_function;
    let values = values.as_slice();
    let parameters = crate::css::serialize::TRANSFORM_FUNCTION_PARAMETER_TYPES[function as usize];

    let axis_needs_reference_box = |index: usize| {
        index < values.len()
            && parameters[index] == TRANSFORM_PARAMETER_LENGTH_PERCENTAGE
            && value_contains_percentage(values[index].data())
    };
    let lower_axis = |index: usize| -> (f32, ComputedStyleValueHandle) {
        if index >= values.len() {
            return (0.0, ComputedStyleValueHandle::empty());
        }
        let value = &values[index];
        if axis_needs_reference_box(index) {
            return (0.0, ComputedStyleValueHandle::retained(value.pointer()));
        }
        (
            length_to_css_pixels(value.data()).to_float(),
            ComputedStyleValueHandle::empty(),
        )
    };
    let translate_entry =
        |x: (f32, ComputedStyleValueHandle), y: (f32, ComputedStyleValueHandle), z: f32| ComputedResolvedTransform {
            is_translate: true,
            matrix: matrix_identity(),
            x_px: x.0,
            y_px: y.0,
            z_px: z,
            x_percentage: x.1,
            y_percentage: y.1,
        };

    match function {
        functions::TRANSLATE => {
            if (values.len() == 1 || values.len() == 2) && (axis_needs_reference_box(0) || axis_needs_reference_box(1))
            {
                return translate_entry(lower_axis(0), lower_axis(1), 0.0);
            }
        }
        functions::TRANSLATE3D => {
            if values.len() == 3 && (axis_needs_reference_box(0) || axis_needs_reference_box(1)) {
                let z = length_to_css_pixels(values[2].data()).to_float();
                return translate_entry(lower_axis(0), lower_axis(1), z);
            }
        }
        functions::TRANSLATE_X => {
            if values.len() == 1 && axis_needs_reference_box(0) {
                return translate_entry(lower_axis(0), (0.0, ComputedStyleValueHandle::empty()), 0.0);
            }
        }
        functions::TRANSLATE_Y if values.len() == 1 && axis_needs_reference_box(0) => {
            return translate_entry((0.0, ComputedStyleValueHandle::empty()), lower_axis(0), 0.0);
        }
        _ => {}
    }

    baked_matrix_entry(transformation_to_matrix(function, values))
}

/// Builds the complete transform payload in Rust, including paint-ready
/// matrices and retained computed-value handles.
unsafe fn build_transform_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::TRANSFORM, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let retained = |property: u16| -> ComputedStyleValueHandle {
        match values.value(property) {
            Some(_) => ComputedStyleValueHandle::retained(values.pointer(property).cast()),
            None => ComputedStyleValueHandle::empty(),
        }
    };

    let mut resolved: Vec<ComputedResolvedTransform> = Vec::new();
    // Pre-lower in the order the transformation matrix accumulates them:
    // translate, rotate, scale, then the transform property's functions.
    let mut individual = |property: u16| -> ComputedStyleValueHandle {
        match values.value(property) {
            Some(data @ StyleValueData::Transformation { .. }) => {
                resolved.push(lower_transformation(data));
                retained(property)
            }
            _ => ComputedStyleValueHandle::empty(),
        }
    };
    let translate = individual(property_id::TRANSLATE);
    let rotate = individual(property_id::ROTATE);
    let scale = individual(property_id::SCALE);

    let transform_list = match values.value(property_id::TRANSFORM) {
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for value in list.as_slice() {
                resolved.push(lower_transformation(value.data()));
            }
            retained(property_id::TRANSFORM)
        }
        _ => ComputedStyleValueHandle::empty(),
    };

    let (transform_origin_x, transform_origin_y, transform_origin_z) = match values.value(property_id::TRANSFORM_ORIGIN)
    {
        Some(StyleValueData::ValueList { values: list, .. }) if list.as_slice().len() == 3 => {
            let offsets = list.as_slice();
            (
                ComputedStyleValueHandle::retained(offsets[0].pointer()),
                ComputedStyleValueHandle::retained(offsets[1].pointer()),
                ComputedStyleValueHandle::retained(offsets[2].pointer()),
            )
        }
        _ => unreachable!("computed transform-origin is a three-value list"),
    };

    let (has_perspective, perspective_px) = match values.value(property_id::PERSPECTIVE) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => (false, 0),
        Some(data) => (true, length_to_css_pixels(data).raw_value()),
        None => (false, 0),
    };

    let Some(StyleValueData::Position { edge_x, edge_y }) = values.value(property_id::PERSPECTIVE_ORIGIN) else {
        unreachable!("computed perspective-origin is a position value");
    };
    let position_offset = |edge: &crate::css::style_value::RetainedStyleValueData| {
        let StyleValueData::Edge { offset, .. } = edge.data() else {
            unreachable!("computed position component is an edge value");
        };
        ComputedStyleValueHandle::retained(offset.pointer())
    };
    let perspective_origin_x = position_offset(edge_x);
    let perspective_origin_y = position_offset(edge_y);

    // SAFETY: The closure casts the scratch payload to its registered
    // Rust-native type and the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::TRANSFORM,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<TransformValues>();
                payload.transformations = transform_list;
                payload.resolved_transforms = RetainedComputedResolvedTransformList::from_vec(resolved);
                payload.transform_origin_x = transform_origin_x;
                payload.transform_origin_y = transform_origin_y;
                payload.transform_origin_z = transform_origin_z;
                payload.rotate = rotate;
                payload.translate = translate;
                payload.scale = scale;
                payload.has_perspective = has_perspective;
                payload.perspective_px = perspective_px;
                payload.perspective_origin_x = perspective_origin_x;
                payload.perspective_origin_y = perspective_origin_y;
            },
            parent_payload,
        )
    }
}

/// The shared prefix of every color style value variant.
fn color_base_of(data: &StyleValueData) -> Option<&crate::css::style_value::ColorBase> {
    match data {
        StyleValueData::ColorFunction { color_base, .. }
        | StyleValueData::ColorMix { color_base, .. }
        | StyleValueData::LightDark { color_base, .. }
        | StyleValueData::ContrastColor { color_base, .. } => Some(color_base),
        _ => None,
    }
}

/// The C++ shadow color-syntax classification: non-color values are legacy,
/// the named color-function types split into the legacy rgb/hsl/hwb family
/// and the modern rest, and untyped colors carry their own syntax flag.
fn shadow_color_syntax(color: Option<&StyleValueData>) -> u8 {
    const COLOR_SYNTAX_LEGACY: u8 = 0;
    const COLOR_SYNTAX_MODERN: u8 = 1;
    // The C++ ColorStyleValue::ColorType codes, pinned by static asserts in
    // ComputedValues.cpp.
    const COLOR_TYPE_RGB: u8 = 0;
    const COLOR_TYPE_HSL: u8 = 4;
    const COLOR_TYPE_HWB: u8 = 5;

    let Some(base) = color.and_then(color_base_of) else {
        return COLOR_SYNTAX_LEGACY;
    };
    if !base.has_color_type {
        return base.color_syntax;
    }
    match base.color_type {
        COLOR_TYPE_RGB | COLOR_TYPE_HSL | COLOR_TYPE_HWB => COLOR_SYNTAX_LEGACY,
        _ => COLOR_SYNTAX_MODERN,
    }
}

/// Lowers one filter property's computed value: the operations bake against
/// the element's colors, and only the url() element lookup stays per paint.
/// Returns the operations plus the retained list handle; a value that is not
/// a filter list lowers to none, as the C++ extractor did.
fn lower_filter_operations(
    values: &EffectiveValues,
    property: u16,
    input: &ColorResolutionInput,
) -> (Vec<ComputedFilterOperation>, ComputedStyleValueHandle) {
    const FILTER_KIND_BLUR: u8 = 0;
    const FILTER_KIND_DROP_SHADOW: u8 = 1;
    const FILTER_KIND_HUE_ROTATE: u8 = 2;
    const FILTER_KIND_COLOR: u8 = 3;
    const FILTER_KIND_URL: u8 = 4;
    // The C++ StyleValueList::Separator::Space code.
    const SEPARATOR_SPACE: u8 = 0;

    let Some(StyleValueData::ValueList {
        values: list,
        separator,
        ..
    }) = values.value(property)
    else {
        return (Vec::new(), ComputedStyleValueHandle::empty());
    };
    let list = list.as_slice();
    // The C++ is_filter_style_value_list check: a non-empty space-separated
    // list of filter functions and url() references.
    if list.is_empty()
        || *separator != SEPARATOR_SPACE
        || !list
            .iter()
            .all(|value| matches!(value.data(), StyleValueData::Filter { .. } | StyleValueData::Url { .. }))
    {
        return (Vec::new(), ComputedStyleValueHandle::empty());
    }

    let empty_operation = || ComputedFilterOperation {
        kind: 0,
        color_operation: 0,
        amount: 0.0,
        shadow_offset_x: 0,
        shadow_offset_y: 0,
        shadow_radius: 0,
        shadow_color: 0,
        url_value: ComputedStyleValueHandle::empty(),
    };
    let mut operations = Vec::with_capacity(list.len());
    for value in list {
        match value.data() {
            StyleValueData::Url { .. } => {
                operations.push(ComputedFilterOperation {
                    kind: FILTER_KIND_URL,
                    url_value: ComputedStyleValueHandle::retained(value.pointer()),
                    ..empty_operation()
                });
            }
            StyleValueData::Filter {
                kind,
                color_operation,
                value: filter_value,
            } => match *kind {
                FILTER_KIND_BLUR => {
                    operations.push(ComputedFilterOperation {
                        kind: FILTER_KIND_BLUR,
                        amount: length_to_px_unrounded(filter_value.data()) as f32,
                        ..empty_operation()
                    });
                }
                FILTER_KIND_DROP_SHADOW => {
                    let StyleValueData::Shadow {
                        color,
                        offset_x,
                        offset_y,
                        blur_radius,
                        ..
                    } = filter_value.data()
                    else {
                        unreachable!("a computed drop-shadow filter holds a shadow value");
                    };
                    let fallback = input.current_color.unwrap_or(Rgba {
                        r: 0,
                        g: 0,
                        b: 0,
                        a: 255,
                    });
                    let resolved_color = color
                        .optional_data()
                        .and_then(|color| to_color(color, input))
                        .unwrap_or(fallback);
                    operations.push(ComputedFilterOperation {
                        kind: FILTER_KIND_DROP_SHADOW,
                        shadow_offset_x: length_to_css_pixels(offset_x.data()).raw_value(),
                        shadow_offset_y: length_to_css_pixels(offset_y.data()).raw_value(),
                        shadow_radius: blur_radius
                            .optional_data()
                            .map_or(0, |radius| length_to_css_pixels(radius).raw_value()),
                        shadow_color: packed_color(resolved_color),
                        ..empty_operation()
                    });
                }
                FILTER_KIND_HUE_ROTATE => {
                    operations.push(ComputedFilterOperation {
                        kind: FILTER_KIND_HUE_ROTATE,
                        amount: angle_degrees(filter_value.data()) as f32,
                        ..empty_operation()
                    });
                }
                FILTER_KIND_COLOR => {
                    operations.push(ComputedFilterOperation {
                        kind: FILTER_KIND_COLOR,
                        color_operation: *color_operation,
                        amount: transform_number(filter_value.data()) as f32,
                        ..empty_operation()
                    });
                }
                _ => unreachable!("the filter kind codes cover every filter function"),
            },
            _ => unreachable!("the filter list was checked above"),
        }
    }
    (
        operations,
        ComputedStyleValueHandle::retained(values.pointer(property).cast()),
    )
}

fn lower_shadow_layers(values: &EffectiveValues, property: u16, input: &ColorResolutionInput) -> Vec<ComputedShadow> {
    let Some(StyleValueData::ValueList { values: list, .. }) = values.value(property) else {
        // A computed shadow list is the none keyword or a value list.
        return Vec::new();
    };
    let mut shadows = Vec::with_capacity(list.as_slice().len());
    for value in list.as_slice() {
        let StyleValueData::Shadow {
            color,
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            placement,
            ..
        } = value.data()
        else {
            unreachable!("a computed box-shadow layer is a shadow value");
        };
        // A missing color is the element's own color, matching the
        // currentcolor default the C++ accessor materialized.
        let resolved_color = match color.optional_data() {
            Some(color) => to_color(color, input).expect("a computed shadow color resolves"),
            None => input.current_color.expect("the build input carries the element color"),
        };
        shadows.push(ComputedShadow {
            offset_x: length_to_css_pixels(offset_x.data()).raw_value(),
            offset_y: length_to_css_pixels(offset_y.data()).raw_value(),
            blur_radius: blur_radius
                .optional_data()
                .map_or(0, |radius| length_to_css_pixels(radius).raw_value()),
            spread_distance: spread_distance
                .optional_data()
                .map_or(0, |spread| length_to_css_pixels(spread).raw_value()),
            color: packed_color(resolved_color),
            color_syntax: shadow_color_syntax(color.optional_data()),
            placement: u32::from(*placement),
        });
    }
    shadows
}

fn lower_clip_edge(data: &StyleValueData) -> ComputedClipEdge {
    match data {
        StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO => ComputedClipEdge {
            is_auto: true,
            value: 0.0,
            unit: crate::css::style_compute::px_length_unit(),
        },
        StyleValueData::Length { value, unit } => ComputedClipEdge {
            is_auto: false,
            value: *value,
            unit: *unit,
        },
        StyleValueData::Calculated { .. } => ComputedClipEdge {
            is_auto: false,
            value: crate::css::calc::resolve_calculated_length_without_context(data, 0.0)
                .expect("a computed clip edge resolves without context"),
            unit: crate::css::style_compute::px_length_unit(),
        },
        _ => unreachable!("a computed clip edge is auto, a length or a calculation"),
    }
}

/// Builds the complete effects payload in Rust.
unsafe fn build_effects_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::EFFECTS, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let (filter_operations, filter_list) = lower_filter_operations(values, property_id::FILTER, input);
    let (backdrop_operations, backdrop_list) = lower_filter_operations(values, property_id::BACKDROP_FILTER, input);
    let shadows = lower_shadow_layers(values, property_id::BOX_SHADOW, input);

    let auto_edge = || ComputedClipEdge {
        is_auto: true,
        value: 0.0,
        unit: crate::css::style_compute::px_length_unit(),
    };
    let (clip_is_rect, clip_edges) = match values.value(property_id::CLIP) {
        Some(StyleValueData::Rect {
            top,
            right,
            bottom,
            left,
        }) => (
            true,
            [
                lower_clip_edge(top.data()),
                lower_clip_edge(right.data()),
                lower_clip_edge(bottom.data()),
                lower_clip_edge(left.data()),
            ],
        ),
        _ => (false, [auto_edge(), auto_edge(), auto_edge(), auto_edge()]),
    };

    let filter = ComputedFilter {
        filter_list,
        operations: RetainedComputedFilterOperationList::from_vec(filter_operations),
    };
    let backdrop_filter = ComputedFilter {
        filter_list: backdrop_list,
        operations: RetainedComputedFilterOperationList::from_vec(backdrop_operations),
    };
    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::EFFECTS,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<EffectsValues>();
                payload.filter = filter;
                payload.backdrop_filter = backdrop_filter;
                payload.box_shadows = RetainedComputedShadowList::from_vec(shadows);
                payload.clip_is_rect = clip_is_rect;
                payload.clip_edges = clip_edges;
                payload.opacity_style_value =
                    ComputedStyleValueHandle::retained(values.pointer(property_id::OPACITY).cast());
                payload.filter_style_value =
                    ComputedStyleValueHandle::retained(values.pointer(property_id::FILTER).cast());
                payload.backdrop_filter_style_value =
                    ComputedStyleValueHandle::retained(values.pointer(property_id::BACKDROP_FILTER).cast());
                payload.mix_blend_mode_style_value =
                    ComputedStyleValueHandle::retained(values.pointer(property_id::MIX_BLEND_MODE).cast());
                payload.isolation_style_value =
                    ComputedStyleValueHandle::retained(values.pointer(property_id::ISOLATION).cast());
                payload.box_shadow_style_value =
                    ComputedStyleValueHandle::retained(values.pointer(property_id::BOX_SHADOW).cast());
                payload.clip_style_value = ComputedStyleValueHandle::retained(values.pointer(property_id::CLIP).cast());
            },
            parent_payload,
        )
    }
}

// --- Border lowering -------------------------------------------------------
//
// The border walks the C++ extractors ran over minted wrappers lower here
// from the table slots. Enum-mapped keywords become their C++ enum codes;
// wrapper-backed members travel as retained canonical handles.

/// The comma-separated computed items of a repeatable-list property: the
/// list's element pointers, or the single value itself.
fn repeatable_item_pointers(values: &EffectiveValues, property: u16) -> Vec<*const c_void> {
    match values.value(property) {
        Some(StyleValueData::ValueList { values: list, .. }) => {
            list.as_slice().iter().map(|value| value.pointer().cast()).collect()
        }
        _ => vec![values.pointer(property)],
    }
}

fn keyword_of(data: &StyleValueData) -> Option<u16> {
    match data {
        StyleValueData::Keyword { keyword } => Some(*keyword),
        _ => None,
    }
}

fn border_radius_is_initial(data: &StyleValueData) -> bool {
    let StyleValueData::BorderRadius {
        horizontal_radius,
        vertical_radius,
        ..
    } = data
    else {
        return false;
    };
    let is_zero_px = |radius: &crate::css::style_value::RetainedStyleValueData| matches!(radius.data(), StyleValueData::Length { value, unit } if *value == 0.0 && *unit == crate::css::style_compute::px_length_unit());
    is_zero_px(horizontal_radius) && is_zero_px(vertical_radius)
}

/// Builds the background group from retained canonical computed values.
unsafe fn build_background_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::BACKGROUND, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };
    let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
    let background_color = resolved_color(
        input,
        property_id::BACKGROUND_COLOR,
        values
            .value(property_id::BACKGROUND_COLOR)
            .expect("background-color has a computed value"),
    )
    .expect("a computed background-color resolves in Rust");
    let image_items = repeatable_item_pointers(values, property_id::BACKGROUND_IMAGE);
    let clip_items = repeatable_item_pointers(values, property_id::BACKGROUND_CLIP);
    let final_clip_pointer = clip_items[(image_items.len() - 1) % clip_items.len()];
    let final_clip = unsafe { &*final_clip_pointer.cast::<StyleValueData>() };
    let background_color_clip = keyword_of(final_clip)
        .and_then(crate::css::css_enums::keyword_to_background_box)
        .expect("a computed background-clip keyword maps to its enum");

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::BACKGROUND,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<BackgroundValues>();
                payload.background_color = background_color;
                payload.background_color_style_value = retained(property_id::BACKGROUND_COLOR);
                payload.background_color_clip = background_color_clip;
                payload.background_image = retained(property_id::BACKGROUND_IMAGE);
                payload.background_attachment = retained(property_id::BACKGROUND_ATTACHMENT);
                payload.background_blend_mode = retained(property_id::BACKGROUND_BLEND_MODE);
                payload.background_clip = retained(property_id::BACKGROUND_CLIP);
                payload.background_origin = retained(property_id::BACKGROUND_ORIGIN);
                payload.background_position_x = retained(property_id::BACKGROUND_POSITION_X);
                payload.background_position_y = retained(property_id::BACKGROUND_POSITION_Y);
                payload.background_repeat = retained(property_id::BACKGROUND_REPEAT);
                payload.background_size = retained(property_id::BACKGROUND_SIZE);
            },
            parent_payload,
        )
    }
}

/// Builds the mask group from retained canonical computed values.
unsafe fn build_mask_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::MASK, values, input, used_color_scheme) }) else {
        return std::ptr::null();
    };
    let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::MASK,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<MaskValues>();
                payload.mask_image = retained(property_id::MASK_IMAGE);
                payload.mask_type = retained(property_id::MASK_TYPE);
                payload.clip_path = retained(property_id::CLIP_PATH);
                payload.mask_mode = retained(property_id::MASK_MODE);
                payload.mask_repeat = retained(property_id::MASK_REPEAT);
                payload.mask_position = retained(property_id::MASK_POSITION);
                payload.mask_clip = retained(property_id::MASK_CLIP);
                payload.mask_origin = retained(property_id::MASK_ORIGIN);
                payload.mask_size = retained(property_id::MASK_SIZE);
                payload.mask_composite = retained(property_id::MASK_COMPOSITE);
            },
            parent_payload,
        )
    }
}

/// Builds the border group from layout-facing side facts and retained
/// canonical values for the C++ presentation views.
unsafe fn build_border_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe { build_generic_group(group_index::BORDER, values, input, used_color_scheme, parent_payload) };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::BORDER, values, input, used_color_scheme) }) else {
        return std::ptr::null();
    };
    let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
    let has_noninitial_border_radii = [
        property_id::BORDER_BOTTOM_LEFT_RADIUS,
        property_id::BORDER_BOTTOM_RIGHT_RADIUS,
        property_id::BORDER_TOP_LEFT_RADIUS,
        property_id::BORDER_TOP_RIGHT_RADIUS,
    ]
    .into_iter()
    .any(|property| !border_radius_is_initial(values.value(property).expect("a border radius has a computed value")));
    let side = |style_property: u16, width_property: u16, color_property: u16| {
        let style_keyword = values
            .value(style_property)
            .and_then(keyword_of)
            .unwrap_or_else(|| unreachable!("a computed border style is a keyword"));
        let line_style = crate::css::css_enums::keyword_to_line_style(style_keyword)
            .unwrap_or_else(|| unreachable!("a computed border style maps to a line style"));
        let computed_width = length_to_css_pixels(
            values
                .value(width_property)
                .expect("a computed border width has a value"),
        );
        crate::css::computed_value_types::ComputedBorderSide {
            color: resolved_color(
                input,
                color_property,
                values
                    .value(color_property)
                    .expect("a computed border color has a value"),
            )
            .expect("a computed border color resolves in Rust"),
            line_style,
            // LineStyle::None and LineStyle::Hidden.
            width: if line_style == 0 || line_style == 1 {
                crate::css::css_pixels::CssPixels::default()
            } else {
                computed_width
            },
        }
    };

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::BORDER,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<BorderValues>();
                payload.border_left = side(
                    property_id::BORDER_LEFT_STYLE,
                    property_id::BORDER_LEFT_WIDTH,
                    property_id::BORDER_LEFT_COLOR,
                );
                payload.border_top = side(
                    property_id::BORDER_TOP_STYLE,
                    property_id::BORDER_TOP_WIDTH,
                    property_id::BORDER_TOP_COLOR,
                );
                payload.border_right = side(
                    property_id::BORDER_RIGHT_STYLE,
                    property_id::BORDER_RIGHT_WIDTH,
                    property_id::BORDER_RIGHT_COLOR,
                );
                payload.border_bottom = side(
                    property_id::BORDER_BOTTOM_STYLE,
                    property_id::BORDER_BOTTOM_WIDTH,
                    property_id::BORDER_BOTTOM_COLOR,
                );
                payload.border_left_color_style_value = retained(property_id::BORDER_LEFT_COLOR);
                payload.border_top_color_style_value = retained(property_id::BORDER_TOP_COLOR);
                payload.border_right_color_style_value = retained(property_id::BORDER_RIGHT_COLOR);
                payload.border_bottom_color_style_value = retained(property_id::BORDER_BOTTOM_COLOR);
                payload.border_left_computed_width = length_to_css_pixels(
                    values
                        .value(property_id::BORDER_LEFT_WIDTH)
                        .expect("border-left-width has a computed value"),
                );
                payload.border_top_computed_width = length_to_css_pixels(
                    values
                        .value(property_id::BORDER_TOP_WIDTH)
                        .expect("border-top-width has a computed value"),
                );
                payload.border_right_computed_width = length_to_css_pixels(
                    values
                        .value(property_id::BORDER_RIGHT_WIDTH)
                        .expect("border-right-width has a computed value"),
                );
                payload.border_bottom_computed_width = length_to_css_pixels(
                    values
                        .value(property_id::BORDER_BOTTOM_WIDTH)
                        .expect("border-bottom-width has a computed value"),
                );
                payload.border_bottom_left_radius = retained(property_id::BORDER_BOTTOM_LEFT_RADIUS);
                payload.border_bottom_right_radius = retained(property_id::BORDER_BOTTOM_RIGHT_RADIUS);
                payload.border_top_left_radius = retained(property_id::BORDER_TOP_LEFT_RADIUS);
                payload.border_top_right_radius = retained(property_id::BORDER_TOP_RIGHT_RADIUS);
                payload.has_noninitial_border_radii = has_noninitial_border_radii;
                payload.border_image_source = retained(property_id::BORDER_IMAGE_SOURCE);
                payload.border_image_slice = retained(property_id::BORDER_IMAGE_SLICE);
                payload.border_image_width = retained(property_id::BORDER_IMAGE_WIDTH);
                payload.border_image_outset = retained(property_id::BORDER_IMAGE_OUTSET);
                payload.border_image_repeat = retained(property_id::BORDER_IMAGE_REPEAT);
            },
            parent_payload,
        )
    }
}

// --- SVG, list and content lowering -----------------------------------------

const SVG_PAINT_NONE: u8 = 0;
const SVG_PAINT_COLOR: u8 = 1;
const SVG_PAINT_URL: u8 = 2;

fn lower_svg_paint(values: &EffectiveValues, property: u16, input: &ColorResolutionInput) -> ComputedSvgPaint {
    let mut paint = ComputedSvgPaint {
        kind: SVG_PAINT_NONE,
        url: ComputedStyleValueHandle::empty(),
        has_color: false,
        color: 0,
        color_is_currentcolor: false,
    };
    let data = values.value(property).expect("the table holds the paint property");
    match data {
        StyleValueData::Keyword { keyword: code } if *code == keyword::NONE => paint,
        StyleValueData::ValueList { values: list, .. } if list.as_slice().len() == 2 => {
            let components = list.as_slice();
            paint.kind = SVG_PAINT_URL;
            paint.url = ComputedStyleValueHandle::retained(components[0].pointer());
            match components[1].data() {
                StyleValueData::EmptyOptional => paint,
                StyleValueData::Keyword { keyword: code } if *code == keyword::NONE => paint,
                fallback => {
                    let color = to_color(fallback, input).expect("a computed SVG paint fallback is a color");
                    paint.has_color = true;
                    paint.color = packed_color(color);
                    paint.color_is_currentcolor =
                        matches!(fallback, StyleValueData::Keyword { keyword: code } if *code == keyword::CURRENTCOLOR);
                    paint
                }
            }
        }
        _ => {
            let color = to_color(data, input).expect("a computed SVG paint is none, a color, or a URL paint");
            paint.kind = SVG_PAINT_COLOR;
            paint.has_color = true;
            paint.color = packed_color(color);
            paint.color_is_currentcolor =
                matches!(data, StyleValueData::Keyword { keyword: code } if *code == keyword::CURRENTCOLOR);
            paint
        }
    }
}

fn lower_svg_length_percentage_or_number(values: &EffectiveValues, property: u16) -> ComputedStyleValueHandle {
    let data = values.value(property).expect("the table holds the property");
    let number = match data {
        StyleValueData::Number { value } => Some(*value),
        data @ StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_number_without_context(data),
        _ => None,
    };
    match number {
        // FIXME: Converting to pixels isn't really correct - values should be in "user units"
        //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
        Some(number) => ComputedStyleValueHandle::length(number),
        None => ComputedStyleValueHandle::retained(values.pointer(property).cast()),
    }
}

/// Builds the complete inherited SVG payload in Rust.
unsafe fn build_inherited_svg_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::INHERITED_SVG, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    // stroke-dasharray: none is the empty list; everything else is a list of
    // lengths, percentages, numbers and calculations.
    let mut dashes: Vec<ComputedSvgDash> = Vec::new();
    if let Some(StyleValueData::ValueList { values: list, .. }) = values.value(property_id::STROKE_DASHARRAY) {
        for item in list.as_slice() {
            let data = item.data();
            let number = match data {
                StyleValueData::Number { value } => Some(*value),
                data @ StyleValueData::Calculated { .. } => {
                    crate::css::calc::resolve_calculated_number_without_context(data)
                }
                _ => None,
            };
            dashes.push(match number {
                Some(number) => ComputedSvgDash {
                    is_number: true,
                    number,
                    value: ComputedStyleValueHandle::empty(),
                },
                None => ComputedSvgDash {
                    is_number: false,
                    number: 0.0,
                    value: ComputedStyleValueHandle::retained(item.pointer()),
                },
            });
        }
    }

    // paint-order, with the omitted-keyword completion the extractor used.
    use crate::css::css_enums::paint_order;
    let mut order = [paint_order::FILL, paint_order::STROKE, paint_order::MARKERS];
    let serialization_length: u8;
    let mut is_normal = false;
    match values.value(property_id::PAINT_ORDER) {
        Some(StyleValueData::Keyword { keyword: code }) => {
            is_normal = *code == keyword::NORMAL;
            serialization_length = if is_normal { 0 } else { 1 };
            if !is_normal {
                match crate::css::css_enums::keyword_to_paint_order(*code)
                    .expect("a computed paint-order keyword maps to its enum")
                {
                    paint_order::FILL => {}
                    paint_order::STROKE => order = [paint_order::STROKE, paint_order::FILL, paint_order::MARKERS],
                    paint_order::MARKERS => order = [paint_order::MARKERS, paint_order::FILL, paint_order::STROKE],
                    _ => unreachable!("the paint-order codes cover every keyword"),
                }
            }
        }
        Some(StyleValueData::ValueList { values: list, .. }) => {
            // The third keyword is omitted by the shortest-serialization
            // principle; the code sum infers it.
            let items = list.as_slice();
            assert!(items.len() == 2, "a computed paint-order list holds two keywords");
            serialization_length = items.len() as u8;
            let mut sum = 0u8;
            for (index, item) in items.iter().enumerate() {
                let code = keyword_of(item.data())
                    .and_then(crate::css::css_enums::keyword_to_paint_order)
                    .expect("a computed paint-order item maps to its enum");
                sum += code;
                order[index] = code;
            }
            assert!(sum <= 3, "paint-order keywords cannot repeat");
            order[2] = 3 - sum;
        }
        _ => unreachable!("a computed paint-order is a keyword or a list"),
    }

    let dominant_baseline = values
        .value(property_id::DOMINANT_BASELINE)
        .and_then(keyword_of)
        .and_then(crate::css::css_enums::keyword_to_baseline_metric);

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::INHERITED_SVG,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<InheritedSVGValues>();
                payload.fill = lower_svg_paint(values, property_id::FILL, input);
                payload.stroke = lower_svg_paint(values, property_id::STROKE, input);
                payload.stroke_dasharray = RetainedComputedSvgDashList::from_vec(dashes);
                payload.stroke_dashoffset =
                    lower_svg_length_percentage_or_number(values, property_id::STROKE_DASHOFFSET);
                payload.stroke_width = lower_svg_length_percentage_or_number(values, property_id::STROKE_WIDTH);
                payload.paint_order = order;
                payload.paint_order_serialization_length = serialization_length;
                payload.paint_order_is_normal = is_normal;
                payload.has_dominant_baseline = dominant_baseline.is_some();
                payload.dominant_baseline = dominant_baseline.unwrap_or(0);
            },
            parent_payload,
        )
    }
}

/// Builds the inherited list group from canonical values without leaving
/// Rust. Counter-style scope resolution remains a consumer-side operation.
unsafe fn build_inherited_list_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::INHERITED_LIST,
            values,
            input,
            used_color_scheme,
            parent_payload,
        )
    };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) =
        (unsafe { gather_group_entries(group_index::INHERITED_LIST, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::INHERITED_LIST,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<InheritedListValues>();
                let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
                payload.list_style_type = retained(property_id::LIST_STYLE_TYPE);
                payload.list_style_position = values
                    .value(property_id::LIST_STYLE_POSITION)
                    .and_then(keyword_of)
                    .and_then(crate::css::css_enums::keyword_to_list_style_position)
                    .expect("a computed list-style-position maps to its enum");
                payload.list_style_image = retained(property_id::LIST_STYLE_IMAGE);
                payload.quotes = retained(property_id::QUOTES);
            },
            parent_payload,
        )
    }
}

/// Builds the content group from its canonical values without leaving Rust.
unsafe fn build_content_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic =
        unsafe { build_generic_group(group_index::CONTENT, values, input, used_color_scheme, parent_payload) };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::CONTENT, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::CONTENT,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<ContentValues>();
                let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
                payload.content = retained(property_id::CONTENT);
                payload.counter_increment = retained(property_id::COUNTER_INCREMENT);
                payload.counter_reset = retained(property_id::COUNTER_RESET);
                payload.counter_set = retained(property_id::COUNTER_SET);
            },
            parent_payload,
        )
    }
}

// --- Inherited UI, inherited text and misc lowering -------------------------

/// Builds the complete inherited UI payload in Rust.
unsafe fn build_inherited_ui_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::INHERITED_UI, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let color_or_auto = |property: u16| -> ComputedColorOrAuto {
        let data = values.value(property).expect("the table holds the color property");
        let is_auto = matches!(data, StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO);
        let used_color = resolved_color(input, property, data).expect("computed UI colors are resolvable in Rust");
        ComputedColorOrAuto {
            is_auto,
            computed_color: used_color,
            used_color,
        }
    };
    let caret_color = color_or_auto(property_id::CARET_COLOR);
    let accent_color = color_or_auto(property_id::ACCENT_COLOR);

    // The cursor list, with the extractor's rules: unmappable keywords are
    // skipped, and an empty result is the predefined auto cursor.
    let mut cursors: Vec<ComputedCursor> = Vec::new();
    let mut push_cursor = |data: &StyleValueData, pointer: *const c_void| match data {
        StyleValueData::Cursor { .. } => cursors.push(ComputedCursor {
            is_cursor_value: true,
            cursor: ComputedStyleValueHandle::retained(pointer.cast()),
            predefined: 0,
        }),
        _ => {
            if let Some(predefined) = keyword_of(data).and_then(crate::css::css_enums::keyword_to_cursor_predefined) {
                cursors.push(ComputedCursor {
                    is_cursor_value: false,
                    cursor: ComputedStyleValueHandle::empty(),
                    predefined,
                });
            }
        }
    };
    match values.value(property_id::CURSOR) {
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for item in list.as_slice() {
                push_cursor(item.data(), item.pointer().cast());
            }
        }
        Some(data) => push_cursor(data, values.pointer(property_id::CURSOR)),
        None => {}
    }
    if cursors.is_empty() {
        cursors.push(ComputedCursor {
            is_cursor_value: false,
            cursor: ComputedStyleValueHandle::empty(),
            predefined: crate::css::css_enums::cursor_predefined::AUTO,
        });
    }

    let scrollbar_color = if let Some(StyleValueData::ScrollbarColor {
        thumb_color,
        track_color,
    }) = values.value(property_id::SCROLLBAR_COLOR)
    {
        ComputedScrollbarColor {
            thumb_color: packed_color(
                to_color(thumb_color.data(), input).expect("computed scrollbar colors are resolvable in Rust"),
            ),
            track_color: packed_color(
                to_color(track_color.data(), input).expect("computed scrollbar colors are resolvable in Rust"),
            ),
            is_auto: false,
        }
    } else {
        ComputedScrollbarColor {
            thumb_color: 0,
            track_color: 0,
            is_auto: true,
        }
    };

    let Some(StyleValueData::ColorScheme { schemes, only, .. }) = values.value(property_id::COLOR_SCHEME) else {
        unreachable!("a computed color-scheme is a color-scheme value");
    };
    let color_schemes = RetainedUtf16FlyStringList::from_retained_strings(schemes.as_slice().to_vec());

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::INHERITED_UI,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<InheritedUIValues>();
                payload.caret_color = caret_color;
                payload.accent_color = accent_color;
                payload.cursor = RetainedComputedCursorList::from_vec(cursors);
                payload.scrollbar_color = scrollbar_color;
                payload.color_schemes = color_schemes;
                payload.color_scheme_only = *only;
            },
            parent_payload,
        )
    }
}

/// Resolves a computed length-percentage against the element's font size.
fn font_relative_length_to_css_pixels(data: &StyleValueData, input: &ColorResolutionInput) -> CssPixels {
    let context = input
        .length
        .expect("inherited text builds with a length resolution context");
    let font_size = context.font_metrics.font_size;
    let pixels = match data {
        StyleValueData::Length { value, unit } => {
            let result = crate::css::style_compute::absolutize_length(*value, *unit as usize, context);
            assert!(result.handled, "a computed inherited-text length is resolvable");
            result.px
        }
        StyleValueData::Percentage { value } => value * font_size / 100.0,
        StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calculated_length_percentage_with_context(data, font_size, context)
                .expect("a computed inherited-text calculation resolves to a length")
        }
        _ => unreachable!("an inherited-text length is a length, percentage, or calculation"),
    };
    CssPixels::nearest_value_for(pixels)
}

/// Builds the complete inherited text payload in Rust.
unsafe fn build_inherited_text_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) =
        (unsafe { gather_group_entries(group_index::INHERITED_TEXT, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let text_shadows = lower_shadow_layers(values, property_id::TEXT_SHADOW, input);

    let Some(StyleValueData::TextUnderlinePosition { horizontal, vertical }) =
        values.value(property_id::TEXT_UNDERLINE_POSITION)
    else {
        unreachable!("a computed text-underline-position is a text-underline-position value");
    };

    let underline_offset_data = values
        .value(property_id::TEXT_UNDERLINE_OFFSET)
        .expect("the table holds text-underline-offset");
    let underline_offset_is_auto =
        matches!(underline_offset_data, StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO);

    let Some(StyleValueData::TextIndent {
        length_percentage,
        hanging,
        each_line,
    }) = values.value(property_id::TEXT_INDENT)
    else {
        unreachable!("a computed text-indent is a text-indent value");
    };

    // tab-size: a number, or a length in CSSPixels, with calculations
    // resolving by their declared type.
    let tab_size_data = values.value(property_id::TAB_SIZE).expect("the table holds tab-size");
    let (tab_size_is_number, tab_size_number, tab_size_px) = match tab_size_data {
        StyleValueData::Number { value } => (true, *value, 0),
        data @ StyleValueData::Calculated { .. } => {
            match crate::css::calc::resolve_calculated_number_without_context(data) {
                Some(number) => (true, number, 0),
                None => (false, 0.0, length_to_css_pixels(data).raw_value()),
            }
        }
        data => (false, 0.0, length_to_css_pixels(data).raw_value()),
    };

    let spacing = |property| {
        let data = values.value(property).expect("the table holds the spacing property");
        if matches!(data, StyleValueData::Keyword { keyword: code } if *code == keyword::NORMAL) {
            CssPixels::default()
        } else {
            font_relative_length_to_css_pixels(data, input)
        }
    };
    let word_spacing = spacing(property_id::WORD_SPACING);
    let letter_spacing = spacing(property_id::LETTER_SPACING);
    let underline_offset_used = if underline_offset_is_auto {
        CssPixels::from_integer(2)
    } else {
        font_relative_length_to_css_pixels(underline_offset_data, input)
    };

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::INHERITED_TEXT,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<InheritedTextValues>();
                payload.text_shadow = RetainedComputedShadowList::from_vec(text_shadows);
                payload.text_underline_position = ComputedTextUnderlinePosition {
                    horizontal: *horizontal,
                    vertical: *vertical,
                };
                payload.text_underline_offset = ComputedTextUnderlineOffset {
                    used_value: underline_offset_used,
                    is_auto: underline_offset_is_auto,
                    value: if underline_offset_is_auto {
                        ComputedStyleValueHandle::empty()
                    } else {
                        ComputedStyleValueHandle::retained(values.pointer(property_id::TEXT_UNDERLINE_OFFSET).cast())
                    },
                };
                payload.text_indent = ComputedTextIndent {
                    length_percentage: ComputedStyleValueHandle::retained(length_percentage.pointer()),
                    each_line: *each_line,
                    hanging: *hanging,
                };
                payload.tab_size_is_number = tab_size_is_number;
                payload.tab_size_number = tab_size_number;
                payload.tab_size_length = CssPixels::from_raw(tab_size_px);
                payload.word_spacing = word_spacing;
                payload.letter_spacing = letter_spacing;
            },
            parent_payload,
        )
    }
}

/// Builds the misc reset group entirely from Rust-owned canonical values.
unsafe fn build_misc_reset_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::MISC_RESET,
            values,
            input,
            used_color_scheme,
            parent_payload,
        )
    };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::MISC_RESET, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let resolve_length = |data: &StyleValueData| -> Option<CssPixels> {
        let pixels = match data {
            StyleValueData::Length { value, unit } => crate::css::style_compute::absolute_length_to_px(*value, *unit),
            StyleValueData::Calculated { .. } => {
                crate::css::calc::resolve_calculated_length_with_context(data, input.length?)
            }
            _ => None,
        }?;
        Some(CssPixels::nearest_value_for(pixels))
    };

    let overflow_clip_margin_side = |property: u16| -> ComputedOverflowClipMarginSide {
        match values.value(property) {
            Some(StyleValueData::OverflowClipMargin {
                has_visual_box,
                visual_box,
                offset,
            }) => ComputedOverflowClipMarginSide {
                has_visual_box: *has_visual_box,
                visual_box: *visual_box,
                offset: resolve_length(offset.data()).unwrap_or(CssPixels::from_raw(0)),
            },
            _ => ComputedOverflowClipMarginSide {
                has_visual_box: false,
                visual_box: 0,
                offset: CssPixels::from_raw(0),
            },
        }
    };

    // The appearance compat keywords normalize to auto for the appearance
    // member and stay raw for computed_appearance.
    use crate::css::css_enums::appearance;
    let computed_appearance = values
        .value(property_id::APPEARANCE)
        .and_then(keyword_of)
        .and_then(crate::css::css_enums::keyword_to_appearance)
        .expect("a computed appearance keyword maps to its enum");
    let normalized_appearance = match computed_appearance {
        appearance::SEARCHFIELD
        | appearance::TEXTAREA
        | appearance::PUSH_BUTTON
        | appearance::SLIDER_HORIZONTAL
        | appearance::CHECKBOX
        | appearance::RADIO
        | appearance::SQUARE_BUTTON
        | appearance::MENULIST
        | appearance::LISTBOX
        | appearance::METER
        | appearance::PROGRESS_BAR
        | appearance::BUTTON => appearance::AUTO,
        other => other,
    };

    let Some(StyleValueData::Position { edge_x, edge_y }) = values.value(property_id::OBJECT_POSITION) else {
        unreachable!("a computed object-position is a position value");
    };
    let position_offset = |edge: &crate::css::style_value::RetainedStyleValueData| -> ComputedStyleValueHandle {
        let StyleValueData::Edge { offset, .. } = edge.data() else {
            unreachable!("a computed position component is an edge value");
        };
        ComputedStyleValueHandle::retained(offset.pointer())
    };

    // touch-action, with the extractor's keyword fan-out.
    let mut allow = [true; 6];
    match values.value(property_id::TOUCH_ACTION) {
        Some(StyleValueData::Keyword { keyword: code }) => match *code {
            keyword::AUTO => {}
            keyword::NONE => allow = [false; 6],
            keyword::MANIPULATION => allow[5] = false,
            _ => unreachable!("a computed single-keyword touch-action is auto, none or manipulation"),
        },
        Some(StyleValueData::ValueList { values: list, .. }) => {
            allow = [false, false, false, false, false, false];
            for item in list.as_slice() {
                match keyword_of(item.data()).expect("a computed touch-action item is a keyword") {
                    keyword::PAN_X => {
                        allow[0] = true;
                        allow[1] = true;
                    }
                    keyword::PAN_LEFT => allow[0] = true,
                    keyword::PAN_RIGHT => allow[1] = true,
                    keyword::PAN_Y => {
                        allow[2] = true;
                        allow[3] = true;
                    }
                    keyword::PAN_UP => allow[2] = true,
                    keyword::PAN_DOWN => allow[3] = true,
                    keyword::PINCH_ZOOM => allow[4] = true,
                    _ => unreachable!("the touch-action keywords cover every list item"),
                }
            }
        }
        _ => {}
    }

    let Some(StyleValueData::ScrollbarGutter {
        value: scrollbar_gutter,
    }) = values.value(property_id::SCROLLBAR_GUTTER)
    else {
        unreachable!("a computed scrollbar-gutter is a scrollbar-gutter value");
    };

    let outline_offset_data = values
        .value(property_id::OUTLINE_OFFSET)
        .expect("the table holds outline-offset");
    let Some(outline_offset) = resolve_length(outline_offset_data) else {
        return std::ptr::null();
    };
    let outline_color = packed_color(
        to_color(
            values
                .value(property_id::OUTLINE_COLOR)
                .expect("the table holds outline-color"),
            input,
        )
        .expect("a computed outline-color resolves in the Rust color context"),
    );

    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::MISC_RESET,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<MiscResetValues>();
                let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
                payload.outline_offset_style_value = retained(property_id::OUTLINE_OFFSET);
                payload.outline_offset = outline_offset;
                payload.scroll_margin = ComputedLengthBox::from_data(
                    values.pointer(property_id::SCROLL_MARGIN_TOP),
                    values.pointer(property_id::SCROLL_MARGIN_RIGHT),
                    values.pointer(property_id::SCROLL_MARGIN_BOTTOM),
                    values.pointer(property_id::SCROLL_MARGIN_LEFT),
                    false,
                );
                payload.scroll_padding = ComputedLengthBox::from_data(
                    values.pointer(property_id::SCROLL_PADDING_TOP),
                    values.pointer(property_id::SCROLL_PADDING_RIGHT),
                    values.pointer(property_id::SCROLL_PADDING_BOTTOM),
                    values.pointer(property_id::SCROLL_PADDING_LEFT),
                    true,
                );
                payload.overflow_clip_margin = ComputedOverflowClipMargin {
                    left: overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_LEFT),
                    top: overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_TOP),
                    right: overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_RIGHT),
                    bottom: overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_BOTTOM),
                };
                payload.appearance = normalized_appearance;
                payload.computed_appearance = computed_appearance;
                payload.column_height = ComputedSize::from_data(values.pointer(property_id::COLUMN_HEIGHT));
                payload.outline_color = outline_color;
                payload.object_position_x = position_offset(edge_x);
                payload.object_position_y = position_offset(edge_y);
                payload.view_transition_name = retained(property_id::VIEW_TRANSITION_NAME);
                payload.touch_action_allow_left = allow[0];
                payload.touch_action_allow_right = allow[1];
                payload.touch_action_allow_up = allow[2];
                payload.touch_action_allow_down = allow[3];
                payload.touch_action_allow_pinch_zoom = allow[4];
                payload.touch_action_allow_other = allow[5];
                payload.scrollbar_gutter = *scrollbar_gutter;
                payload.shape_margin = retained(property_id::SHAPE_MARGIN);
                payload.shape_outside = retained(property_id::SHAPE_OUTSIDE);
                payload.will_change = retained(property_id::WILL_CHANGE);
            },
            parent_payload,
        )
    }
}

/// The C++ StyleValueList::Separator::Comma discriminant, static-asserted in
/// ComputedValues.cpp.
const SEPARATOR_COMMA: u8 = 1;

/// Builds the complete text reset group in Rust.
unsafe fn build_text_reset_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    parent_payload: *const c_void,
) -> *const c_void {
    let mut text_decoration_lines: Vec<u8> = Vec::new();
    match values.value(property_id::TEXT_DECORATION_LINE) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => {}
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for item in list.as_slice() {
                text_decoration_lines.push(
                    keyword_of(item.data())
                        .and_then(crate::css::css_enums::keyword_to_text_decoration_line)
                        .expect("a computed text-decoration-line item maps to its enum"),
                );
            }
        }
        _ => unreachable!("a computed text-decoration-line is none or a value list"),
    }

    let (thickness_kind, thickness_value) = match values.value(property_id::TEXT_DECORATION_THICKNESS) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::AUTO => (0u8, std::ptr::null()),
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::FROM_FONT => (1u8, std::ptr::null()),
        Some(_) => (2u8, values.pointer(property_id::TEXT_DECORATION_THICKNESS)),
        None => unreachable!("the table holds text-decoration-thickness"),
    };

    let mut discard_before = false;
    let mut discard_after = false;
    let mut discard_inner = false;
    match values.value(property_id::WHITE_SPACE_TRIM) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => {}
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for item in list.as_slice() {
                match keyword_of(item.data()) {
                    Some(keyword::DISCARD_BEFORE) => discard_before = true,
                    Some(keyword::DISCARD_AFTER) => discard_after = true,
                    Some(keyword::DISCARD_INNER) => discard_inner = true,
                    _ => unreachable!("the white-space-trim keywords cover every list item"),
                }
            }
        }
        _ => unreachable!("a computed white-space-trim is none or a value list"),
    }

    let text_decoration_style = keyword_of(
        values
            .value(property_id::TEXT_DECORATION_STYLE)
            .expect("the table holds text-decoration-style"),
    )
    .and_then(crate::css::css_enums::keyword_to_text_decoration_style)
    .expect("a computed text-decoration-style maps to its enum");
    let text_decoration_color = packed_color(
        to_color(
            values
                .value(property_id::TEXT_DECORATION_COLOR)
                .expect("the table holds text-decoration-color"),
            input,
        )
        .expect("a computed text-decoration-color resolves in the Rust color context"),
    );

    // SAFETY: The list and value pointers remain live through the call and
    // the caller warrants the parent payload.
    unsafe {
        rust_build_text_reset_group(
            group_index::TEXT_RESET,
            text_decoration_lines.as_ptr(),
            text_decoration_lines.len(),
            thickness_kind,
            thickness_value,
            text_decoration_style,
            text_decoration_color,
            discard_before,
            discard_after,
            discard_inner,
            parent_payload,
        )
    }
}

/// The C++ PositionArea codes a value's keywords map to, fanning out a value
/// list and skipping non-area keywords like the extractor's helper.
fn position_area_codes(data: &StyleValueData) -> Vec<u8> {
    let mut codes = Vec::new();
    let mut append = |data: &StyleValueData| {
        if let Some(code) = keyword_of(data).and_then(crate::css::css_enums::keyword_to_position_area) {
            codes.push(code);
        }
    };
    match data {
        StyleValueData::ValueList { values: list, .. } => {
            for item in list.as_slice() {
                append(item.data());
            }
        }
        data => append(data),
    }
    codes
}

/// Builds the complete anchor-positioning payload in Rust.
unsafe fn build_anchor_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe { build_generic_group(group_index::ANCHOR, values, input, used_color_scheme, parent_payload) };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::ANCHOR, values, input, used_color_scheme) }) else {
        return std::ptr::null();
    };

    let custom_idents = |property| {
        let mut names = Vec::new();
        let mut append = |data: &StyleValueData| {
            if let StyleValueData::CustomIdent { custom_ident } = data {
                names.push(custom_ident.clone());
            }
        };
        match values.value(property) {
            Some(StyleValueData::ValueList { values: list, .. }) => {
                for item in list.as_slice() {
                    append(item.data());
                }
            }
            Some(data) => append(data),
            None => {}
        }
        names
    };
    let anchor_names = custom_idents(property_id::ANCHOR_NAME);
    let anchor_scope_all = matches!(
        values.value(property_id::ANCHOR_SCOPE),
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::ALL
    );
    let anchor_scope_names = custom_idents(property_id::ANCHOR_SCOPE);

    let (position_anchor_type, position_anchor_name) = match values.value(property_id::POSITION_ANCHOR) {
        Some(StyleValueData::CustomIdent { custom_ident }) => (3u8, custom_ident.clone()),
        Some(StyleValueData::Keyword { keyword: code }) => match *code {
            keyword::NORMAL => (0u8, RetainedUtf16FlyString::none()),
            keyword::NONE => (1u8, RetainedUtf16FlyString::none()),
            keyword::AUTO => (2u8, RetainedUtf16FlyString::none()),
            _ => unreachable!("a computed position-anchor keyword is normal, none or auto"),
        },
        _ => unreachable!("a computed position-anchor is a keyword or a custom ident"),
    };

    let position_area_keywords = position_area_codes(
        values
            .value(property_id::POSITION_AREA)
            .expect("the table holds position-area"),
    );

    // position-try-fallbacks, with the extractor's item rules: an item whose
    // keywords form a position area keeps only the area, and everything else
    // fans out (one level deep) into a name and try tactics.
    let mut position_try_fallbacks: Vec<ComputedPositionTryFallback> = Vec::new();
    let mut append_fallback = |data: &StyleValueData| {
        let mut fallback = ComputedPositionTryFallback {
            name: RetainedUtf16FlyString::none(),
            tactics: [0; 3],
            tactic_count: 0,
            has_position_area: false,
            position_area: RetainedPositionAreaList::from_vec(Vec::new()),
        };
        let apply_item = |fallback: &mut ComputedPositionTryFallback, data: &StyleValueData| {
            if let StyleValueData::CustomIdent { custom_ident } = data {
                fallback.name = custom_ident.clone();
            } else if let Some(tactic) = keyword_of(data).and_then(crate::css::css_enums::keyword_to_try_tactic) {
                assert!(
                    fallback.tactic_count < 3,
                    "a try tactic list holds at most three tactics"
                );
                fallback.tactics[fallback.tactic_count] = tactic;
                fallback.tactic_count += 1;
            }
        };
        let area = position_area_codes(data);
        if !area.is_empty() {
            fallback.has_position_area = true;
            fallback.position_area = RetainedPositionAreaList::from_vec(area);
        } else if let StyleValueData::ValueList { values: list, .. } = data {
            for item in list.as_slice() {
                if let StyleValueData::ValueList { values: children, .. } = item.data() {
                    for child in children.as_slice() {
                        apply_item(&mut fallback, child.data());
                    }
                } else {
                    apply_item(&mut fallback, item.data());
                }
            }
        } else {
            apply_item(&mut fallback, data);
        }
        position_try_fallbacks.push(fallback);
    };
    match values.value(property_id::POSITION_TRY_FALLBACKS) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => {}
        Some(StyleValueData::ValueList {
            values: list,
            separator,
            ..
        }) if *separator == SEPARATOR_COMMA => {
            for item in list.as_slice() {
                append_fallback(item.data());
            }
        }
        Some(data) => append_fallback(data),
        None => unreachable!("the table holds position-try-fallbacks"),
    }

    let position_try_order = match values.value(property_id::POSITION_TRY_ORDER) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NORMAL => None,
        Some(data) => keyword_of(data).and_then(crate::css::css_enums::keyword_to_try_order),
        None => unreachable!("the table holds position-try-order"),
    };

    // position-visibility, with the extractor's keyword fan-out; the
    // anchors-visible default flips to false before the keywords apply.
    let mut always = false;
    let mut anchors_valid = false;
    let mut anchors_visible = false;
    let mut no_overflow = false;
    {
        let mut apply_keyword = |code: u16| match code {
            keyword::ANCHORS_VALID => anchors_valid = true,
            keyword::ANCHORS_VISIBLE => anchors_visible = true,
            keyword::NO_OVERFLOW => no_overflow = true,
            _ => {}
        };
        match values.value(property_id::POSITION_VISIBILITY) {
            Some(StyleValueData::Keyword { keyword: code }) => {
                always = *code == keyword::ALWAYS;
                apply_keyword(*code);
            }
            Some(StyleValueData::ValueList { values: list, .. }) => {
                for item in list.as_slice() {
                    if let Some(code) = keyword_of(item.data()) {
                        apply_keyword(code);
                    }
                }
            }
            _ => unreachable!("a computed position-visibility is a keyword or a value list"),
        }
    }

    let built = AnchorValues {
        anchor_names: RetainedUtf16FlyStringList::from_retained_strings(anchor_names),
        anchor_scope_all,
        anchor_scope_names: RetainedUtf16FlyStringList::from_retained_strings(anchor_scope_names),
        position_anchor_type,
        position_anchor_name,
        position_area: RetainedPositionAreaList::from_vec(position_area_keywords),
        position_try_fallbacks: RetainedPositionTryFallbackList::from_vec(position_try_fallbacks),
        has_position_try_order: position_try_order.is_some(),
        position_try_order: position_try_order.unwrap_or(0),
        position_visibility_always: always,
        position_visibility_anchors_valid: anchors_valid,
        position_visibility_anchors_visible: anchors_visible,
        position_visibility_no_overflow: no_overflow,
    };
    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::ANCHOR,
            &entries,
            |payload| *payload.cast::<AnchorValues>() = built,
            parent_payload,
        )
    }
}

/// The keyword code of a slot that must hold an enum keyword.
fn required_keyword_code(values: &EffectiveValues, property: u16, map: fn(u16) -> Option<u8>) -> u8 {
    values
        .value(property)
        .and_then(keyword_of)
        .and_then(map)
        .expect("a computed enum-keyword slot maps to its enum")
}

/// The resolved integer of an integer-or-calc slot, mirroring the C++
/// int_from_style_value.
fn required_integer(data: &StyleValueData) -> i32 {
    match data {
        StyleValueData::Integer { value } => *value,
        data @ StyleValueData::Calculated { .. } => resolve_calculated_integer_without_context(data)
            .expect("a computed integer calculation resolves without context"),
        _ => unreachable!("a computed integer slot holds an integer or a calculation"),
    }
}

/// The resolved number of a number-or-calc value, mirroring the C++
/// number_from_style_value without a percentage basis.
fn required_number(data: &StyleValueData) -> f64 {
    match data {
        StyleValueData::Number { value } => *value,
        data @ StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_number_without_context(data)
            .expect("a computed number calculation resolves without context"),
        _ => unreachable!("a computed number slot holds a number or a calculation"),
    }
}

/// Builds the box group's payload from the table slots. The box type
/// transformation happened at compute time, so the display slot already
/// holds the transformed value; only the pre-transformation display is a
/// C++-side member and arrives through the build inputs.
unsafe fn build_box_group(
    values: &EffectiveValues,
    display_before_transformation_raw: u32,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::computed_value_types::{BoxValues, ComputedAspectRatio, ComputedVerticalAlign};
    use crate::css::css_enums::{
        keyword_to_box_sizing, keyword_to_clear, keyword_to_float, keyword_to_overflow, keyword_to_positioning,
        keyword_to_resize, keyword_to_table_layout, keyword_to_text_overflow, keyword_to_unicode_bidi,
        keyword_to_vertical_align,
    };

    let display = match values.value(property_id::DISPLAY) {
        Some(StyleValueData::Display { raw }) => crate::css::display::FfiDisplay::from_raw(*raw),
        _ => unreachable!("a computed display is a display value"),
    };

    let grid_auto_flow = match values.value(property_id::GRID_AUTO_FLOW) {
        Some(StyleValueData::GridAutoFlow { row, dense }) => (*row, *dense),
        _ => (true, false),
    };

    let column_count = match values.value(property_id::COLUMN_COUNT) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::AUTO => None,
        Some(data) => Some(required_integer(data)),
        None => unreachable!("the table holds column-count"),
    };

    let z_index = match values.value(property_id::Z_INDEX) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::AUTO => None,
        Some(data) => Some(required_integer(data)),
        None => unreachable!("the table holds z-index"),
    };

    let vertical_align = match values.value(property_id::VERTICAL_ALIGN) {
        Some(StyleValueData::Keyword { keyword: code }) => ComputedVerticalAlign {
            is_keyword: true,
            keyword: keyword_to_vertical_align(*code).expect("a computed vertical-align keyword maps to its enum"),
            value: ComputedStyleValueHandle::empty(),
        },
        Some(_) => ComputedVerticalAlign {
            is_keyword: false,
            keyword: 0,
            value: ComputedStyleValueHandle::retained(values.pointer(property_id::VERTICAL_ALIGN).cast()),
        },
        None => unreachable!("the table holds vertical-align"),
    };

    // aspect-ratio, with the extractor's degenerate-ratio handling; absent
    // ratios keep zeroed components so payload equality stays field-wise.
    let resolved_ratio = |data: &StyleValueData| -> (f64, f64) {
        let StyleValueData::Ratio { numerator, denominator } = data else {
            unreachable!("a computed aspect-ratio component is a ratio");
        };
        (required_number(numerator.data()), required_number(denominator.data()))
    };
    let ratio_is_degenerate = |(numerator, denominator): (f64, f64)| {
        !numerator.is_finite() || numerator == 0.0 || !denominator.is_finite() || denominator == 0.0
    };
    let initial_aspect_ratio = ComputedAspectRatio {
        use_natural_aspect_ratio_if_available: true,
        has_preferred_ratio: false,
        preferred_ratio_numerator: 0.0,
        preferred_ratio_denominator: 0.0,
        computed_use_natural_aspect_ratio_if_available: true,
        has_computed_ratio: false,
        computed_ratio_numerator: 0.0,
        computed_ratio_denominator: 0.0,
    };
    let aspect_ratio = match values.value(property_id::ASPECT_RATIO) {
        Some(StyleValueData::ValueList { values: list, .. }) => {
            let items = list.as_slice();
            let auto_then_ratio = items.len() == 2
                && matches!(items[0].data(), StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO)
                && matches!(items[1].data(), StyleValueData::Ratio { .. });
            if auto_then_ratio {
                let ratio = resolved_ratio(items[1].data());
                ComputedAspectRatio {
                    use_natural_aspect_ratio_if_available: true,
                    has_preferred_ratio: !ratio_is_degenerate(ratio),
                    preferred_ratio_numerator: if ratio_is_degenerate(ratio) { 0.0 } else { ratio.0 },
                    preferred_ratio_denominator: if ratio_is_degenerate(ratio) { 0.0 } else { ratio.1 },
                    computed_use_natural_aspect_ratio_if_available: true,
                    has_computed_ratio: true,
                    computed_ratio_numerator: ratio.0,
                    computed_ratio_denominator: ratio.1,
                }
            } else {
                initial_aspect_ratio
            }
        }
        Some(data @ StyleValueData::Ratio { .. }) => {
            let ratio = resolved_ratio(data);
            let degenerate = ratio_is_degenerate(ratio);
            ComputedAspectRatio {
                use_natural_aspect_ratio_if_available: degenerate,
                has_preferred_ratio: !degenerate,
                preferred_ratio_numerator: if degenerate { 0.0 } else { ratio.0 },
                preferred_ratio_denominator: if degenerate { 0.0 } else { ratio.1 },
                computed_use_natural_aspect_ratio_if_available: false,
                has_computed_ratio: true,
                computed_ratio_numerator: ratio.0,
                computed_ratio_denominator: ratio.1,
            }
        }
        _ => initial_aspect_ratio,
    };

    // contain, with the extractor's keyword fan-out.
    let mut size_containment = false;
    let mut inline_size_containment = false;
    let mut layout_containment = false;
    let mut style_containment = false;
    let mut paint_containment = false;
    {
        let mut apply_containment_keyword = |code: u16| match code {
            keyword::SIZE => size_containment = true,
            keyword::INLINE_SIZE => inline_size_containment = true,
            keyword::LAYOUT => layout_containment = true,
            keyword::STYLE => style_containment = true,
            keyword::PAINT => paint_containment = true,
            _ => {}
        };
        match values.value(property_id::CONTAIN) {
            Some(StyleValueData::Keyword { keyword: code }) => match *code {
                keyword::NONE => {}
                keyword::STRICT => {
                    size_containment = true;
                    layout_containment = true;
                    paint_containment = true;
                    style_containment = true;
                }
                keyword::CONTENT => {
                    layout_containment = true;
                    paint_containment = true;
                    style_containment = true;
                }
                other => apply_containment_keyword(other),
            },
            Some(StyleValueData::ValueList { values: list, .. }) => {
                for item in list.as_slice() {
                    if let Some(code) = keyword_of(item.data()) {
                        apply_containment_keyword(code);
                    }
                }
            }
            _ => {}
        }
    }

    // container-type, which the extractor only fans out from a value list.
    let mut is_size_container = false;
    let mut is_inline_size_container = false;
    let mut is_scroll_state_container = false;
    if let Some(StyleValueData::ValueList { values: list, .. }) = values.value(property_id::CONTAINER_TYPE) {
        for item in list.as_slice() {
            match keyword_of(item.data()) {
                Some(keyword::SIZE) => is_size_container = true,
                Some(keyword::INLINE_SIZE) => is_inline_size_container = true,
                Some(keyword::SCROLL_STATE) => is_scroll_state_container = true,
                _ => {}
            }
        }
    }

    let mut container_names: Vec<RetainedUtf16FlyString> = Vec::new();
    match values.value(property_id::CONTAINER_NAME) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => {}
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for item in list.as_slice() {
                let StyleValueData::CustomIdent { custom_ident } = item.data() else {
                    unreachable!("a computed container-name item is a custom ident");
                };
                container_names.push(custom_ident.clone());
            }
        }
        Some(StyleValueData::CustomIdent { custom_ident }) => container_names.push(custom_ident.clone()),
        _ => unreachable!("a computed container-name is none or custom idents"),
    }

    let built = std::mem::ManuallyDrop::new(BoxValues {
        display,
        display_before_box_type_transformation: crate::css::display::FfiDisplay::from_raw(
            display_before_transformation_raw,
        ),
        float_: required_keyword_code(values, property_id::FLOAT, keyword_to_float),
        clear: required_keyword_code(values, property_id::CLEAR, keyword_to_clear),
        position: required_keyword_code(values, property_id::POSITION, keyword_to_positioning),
        overflow_x: required_keyword_code(values, property_id::OVERFLOW_X, keyword_to_overflow),
        overflow_y: required_keyword_code(values, property_id::OVERFLOW_Y, keyword_to_overflow),
        box_sizing: required_keyword_code(values, property_id::BOX_SIZING, keyword_to_box_sizing),
        resize: required_keyword_code(values, property_id::RESIZE, keyword_to_resize),
        text_overflow: required_keyword_code(values, property_id::TEXT_OVERFLOW, keyword_to_text_overflow),
        unicode_bidi: required_keyword_code(values, property_id::UNICODE_BIDI, keyword_to_unicode_bidi),
        table_layout: required_keyword_code(values, property_id::TABLE_LAYOUT, keyword_to_table_layout),
        grid_auto_flow_row: grid_auto_flow.0,
        grid_auto_flow_dense: grid_auto_flow.1,
        column_width: ComputedSize::from_data(values.pointer(property_id::COLUMN_WIDTH)),
        column_count_has_value: column_count.is_some(),
        column_count: column_count.unwrap_or(0),
        has_z_index: z_index.is_some(),
        z_index: z_index.unwrap_or(0),
        vertical_align,
        aspect_ratio,
        size_containment,
        inline_size_containment,
        layout_containment,
        style_containment,
        paint_containment,
        is_size_container,
        is_inline_size_container,
        is_scroll_state_container,
        container_name: RetainedUtf16FlyStringList::from_retained_strings(container_names),
    });
    // SAFETY: The payload value is fully materialized and ownership of its
    // retained references transfers to the builder; the caller warrants the
    // parent payload.
    unsafe {
        crate::css::computed_values::rust_build_box_group(group_index::BOX, (&raw const built).cast(), parent_payload)
    }
}

/// Builds the inherited font group from canonical Rust longhands and the
/// platform font resources supplied by C++.
unsafe fn build_font_group(
    values: &EffectiveValues,
    inputs: &FfiFontGroupBuildInputs,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::css_pixels::CssPixels;

    assert!(!inputs.first_available_font.is_null());
    assert!(!inputs.font_cascade_list.is_null());
    let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
    let built = FontValues {
        font_size: CssPixels::from_raw(inputs.font_size_raw),
        line_height_used: CssPixels::from_raw(inputs.line_height_used_raw),
        font_variant_emoji: inputs.font_variant_emoji,
        font_ascent: inputs.font_ascent,
        font_descent: inputs.font_descent,
        font_x_height: inputs.font_x_height,
        first_available_font: inputs.first_available_font,
        font_cascade_list: inputs.font_cascade_list,
        font_weight: inputs.font_weight,
        font_width: inputs.font_width,
        math_shift: inputs.math_shift,
        math_style: inputs.math_style,
        math_depth: inputs.math_depth,
        font_family: retained(property_id::FONT_FAMILY),
        font_style: retained(property_id::FONT_STYLE),
        font_optical_sizing: retained(property_id::FONT_OPTICAL_SIZING),
        font_feature_settings: retained(property_id::FONT_FEATURE_SETTINGS),
        font_kerning: retained(property_id::FONT_KERNING),
        font_language_override: retained(property_id::FONT_LANGUAGE_OVERRIDE),
        font_variant_alternates: retained(property_id::FONT_VARIANT_ALTERNATES),
        font_variant_caps: retained(property_id::FONT_VARIANT_CAPS),
        font_variant_east_asian: retained(property_id::FONT_VARIANT_EAST_ASIAN),
        font_variant_ligatures: retained(property_id::FONT_VARIANT_LIGATURES),
        font_variant_numeric: retained(property_id::FONT_VARIANT_NUMERIC),
        font_variant_position: retained(property_id::FONT_VARIANT_POSITION),
        font_variation_settings: retained(property_id::FONT_VARIATION_SETTINGS),
        text_rendering: retained(property_id::TEXT_RENDERING),
        line_height: retained(property_id::LINE_HEIGHT),
        math_shift_value: retained(property_id::MATH_SHIFT),
        math_style_value: retained(property_id::MATH_STYLE),
        math_depth_value: retained(property_id::MATH_DEPTH),
        font_size_value: retained(property_id::FONT_SIZE),
    };
    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::FONT,
            &[],
            |payload| *payload.cast::<FontValues>() = built,
            parent_payload,
        )
    }
}

/// Builds the animation group from the canonical Rust longhand values.
unsafe fn build_animation_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let Some(entries) = (unsafe { gather_group_entries(group_index::ANIMATION, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };
    let retained = |property| ComputedStyleValueHandle::retained(values.pointer(property).cast());
    let is_single_zero_time = |property| {
        let value = unsafe { &*values.pointer(property).cast::<StyleValueData>() };
        let StyleValueData::ValueList { values, .. } = value else {
            return false;
        };
        let [item] = values.as_slice() else {
            return false;
        };
        matches!(item.data(), StyleValueData::Time { value, .. } if *value == 0.0)
    };
    unsafe {
        crate::css::computed_values::build_group_payload_with_rust_fill(
            group_index::ANIMATION,
            &entries,
            |payload| {
                let payload = &mut *payload.cast::<AnimationValues>();
                payload.animation_name = retained(property_id::ANIMATION_NAME);
                payload.animation_composition = retained(property_id::ANIMATION_COMPOSITION);
                payload.animation_delay = retained(property_id::ANIMATION_DELAY);
                payload.animation_direction = retained(property_id::ANIMATION_DIRECTION);
                payload.animation_duration = retained(property_id::ANIMATION_DURATION);
                payload.animation_fill_mode = retained(property_id::ANIMATION_FILL_MODE);
                payload.animation_iteration_count = retained(property_id::ANIMATION_ITERATION_COUNT);
                payload.animation_play_state = retained(property_id::ANIMATION_PLAY_STATE);
                payload.animation_timeline = retained(property_id::ANIMATION_TIMELINE);
                payload.animation_timing_function = retained(property_id::ANIMATION_TIMING_FUNCTION);
                payload.scroll_timeline_name = retained(property_id::SCROLL_TIMELINE_NAME);
                payload.scroll_timeline_axis = retained(property_id::SCROLL_TIMELINE_AXIS);
                payload.timeline_scope = retained(property_id::TIMELINE_SCOPE);
                payload.view_timeline_name = retained(property_id::VIEW_TIMELINE_NAME);
                payload.view_timeline_axis = retained(property_id::VIEW_TIMELINE_AXIS);
                payload.view_timeline_inset = retained(property_id::VIEW_TIMELINE_INSET);
                payload.transition_property = retained(property_id::TRANSITION_PROPERTY);
                payload.transition_duration = retained(property_id::TRANSITION_DURATION);
                payload.transition_timing_function = retained(property_id::TRANSITION_TIMING_FUNCTION);
                payload.transition_delay = retained(property_id::TRANSITION_DELAY);
                payload.transition_behavior = retained(property_id::TRANSITION_BEHAVIOR);
                payload.transition_delay_and_duration_are_single_zero =
                    is_single_zero_time(property_id::TRANSITION_DELAY)
                        && is_single_zero_time(property_id::TRANSITION_DURATION);
            },
            parent_payload,
        )
    }
}

/// Builds every applied group's payload from the longhand table, writing one
/// payload (or null) per group into `out_payloads` and returning the mask of
/// applied groups whose payload the C++ population path must build instead.
/// Non-null payloads carry one reference for the caller, with the marshalled
/// builders' sharing rules.
///
/// # Safety
/// `table` must be a valid frozen table holding the style's computed values;
/// `parent_payloads` and `out_payloads` must each hold `group_count` entries,
/// with each parent entry a valid payload of its group or null; `inputs` must
/// be valid with its pointers live across the call, and its override values
/// must point at live style value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_group_payloads_from_table(
    table: *const ComputedLonghandTable,
    groups_to_apply: u32,
    parent_payloads: *const *const c_void,
    inputs: *const FfiTableGroupBuildInputs,
    out_payloads: *mut *const c_void,
    group_count: usize,
) -> u32 {
    abort_on_panic(|| {
        assert_eq!(
            group_count,
            group_index::COUNT,
            "the C++ StyleGroupIndex numbering drifted from the table builder's mirror"
        );
        let table = unsafe { &*table };
        let inputs = unsafe { &*inputs };
        let parents = unsafe { std::slice::from_raw_parts(parent_payloads, group_count) };
        let out = unsafe { std::slice::from_raw_parts_mut(out_payloads, group_count) };
        let values = EffectiveValues {
            table,
            override_properties: unsafe {
                std::slice::from_raw_parts(inputs.override_properties, inputs.override_count)
            },
            override_values: unsafe { std::slice::from_raw_parts(inputs.override_values, inputs.override_count) },
        };
        let color_input = unsafe { &*inputs.color_input.cast::<FfiColorResolutionInput>() };
        let channels = relative_color_context_from_ffi(color_input);
        // SAFETY: The caller keeps the input's pointers live across the call.
        let input = unsafe { resolution_input_from_ffi(color_input, &channels) };

        let mut needs_cpp_mask = 0u32;
        for group in 0..group_count {
            out[group] = std::ptr::null();
            if (groups_to_apply >> group) & 1 == 0 {
                continue;
            }
            let parent_payload = parents[group];
            // SAFETY: Gathered pointers name live table or override data and
            // the caller warrants the parent payloads.
            let payload = unsafe {
                match group {
                    group_index::FONT => build_font_group(
                        &values,
                        inputs
                            .font
                            .as_ref()
                            .expect("an applied font group has platform font inputs"),
                        parent_payload,
                    ),
                    group_index::BOX => {
                        build_box_group(&values, inputs.box_display_before_transformation_raw, parent_payload)
                    }
                    group_index::INHERITED_TABLE => rust_build_inherited_table_group(
                        group,
                        values.pointer(property_id::BORDER_COLLAPSE),
                        values.pointer(property_id::CAPTION_SIDE),
                        values.pointer(property_id::EMPTY_CELLS),
                        values.pointer(property_id::BORDER_SPACING),
                        parent_payload,
                    ),
                    group_index::INHERITED_BOX => rust_build_inherited_box_group(
                        group,
                        values.pointer(property_id::VISIBILITY),
                        values.pointer(property_id::DIRECTION),
                        values.pointer(property_id::WRITING_MODE),
                        values.pointer(property_id::CONTENT_VISIBILITY),
                        values.pointer(property_id::IMAGE_RENDERING),
                        parent_payload,
                    ),
                    group_index::SIZING => rust_build_sizing_group(
                        group,
                        values.pointer(property_id::WIDTH),
                        values.pointer(property_id::MIN_WIDTH),
                        values.pointer(property_id::MAX_WIDTH),
                        values.pointer(property_id::HEIGHT),
                        values.pointer(property_id::MIN_HEIGHT),
                        values.pointer(property_id::MAX_HEIGHT),
                        parent_payload,
                    ),
                    group_index::SURROUND => build_surround_group(&values, parent_payload),
                    group_index::ANIMATION => {
                        build_animation_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::ANCHOR => {
                        build_anchor_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::TEXT_RESET => build_text_reset_group(&values, &input, parent_payload),
                    group_index::ALIGNMENT => build_alignment_group(&values, parent_payload),
                    group_index::SVG_RESET => build_svg_reset_group(&values, &input, parent_payload),
                    group_index::GRID => build_grid_group(&values, parent_payload),
                    group_index::TRANSFORM => {
                        build_transform_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::EFFECTS => {
                        build_effects_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::INHERITED_UI => {
                        build_inherited_ui_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::INHERITED_TEXT => {
                        build_inherited_text_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::MISC_RESET => {
                        build_misc_reset_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::INHERITED_SVG => {
                        build_inherited_svg_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::INHERITED_LIST => {
                        build_inherited_list_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::CONTENT => {
                        build_content_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::BACKGROUND => {
                        build_background_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::MASK => build_mask_group(&values, &input, inputs.used_color_scheme, parent_payload),
                    group_index::BORDER => {
                        build_border_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    _ => build_generic_group(group, &values, &input, inputs.used_color_scheme, parent_payload),
                }
            };
            out[group] = payload;
            if payload.is_null() {
                needs_cpp_mask |= 1 << group;
            }
        }
        needs_cpp_mask
    })
}
