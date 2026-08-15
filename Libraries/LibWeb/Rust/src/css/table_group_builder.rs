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
    ComputedGridArea, ComputedGridPlacement, ComputedGridPlacementKind, ComputedGridTrackBreadth,
    ComputedGridTrackEntry, ComputedGridTrackEntryKind, ComputedGridTrackList, ComputedSize, ComputedSizeKind,
    ComputedStyleValueHandle, GRID_NO_INDEX, GridValues, RetainedGridAreaList, RetainedGridNameIndexList,
    RetainedGridTrackEntryList,
};
use crate::css::computed_values::{
    FfiGroupValueEntry, GROUP_FIELD_COLOR, GROUP_FIELD_COLOR_OR_KEYWORD, GROUP_FIELD_RESOLVED_F32,
    GROUP_FIELD_RESOLVED_F64, GROUP_FIELD_RESOLVED_U8, registered_field_descriptors, rust_build_alignment_group,
    rust_build_grid_group, rust_build_inherited_box_group, rust_build_inherited_table_group, rust_build_sizing_group,
    rust_build_style_group, rust_build_surround_group, rust_build_svg_reset_group,
};
use crate::css::css_enums::keyword;
use crate::css::css_pixels::CssPixels;
use crate::css::property_metadata::property_id;
use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};
use crate::css::style_value::{GridTrackEntryKind, RetainedGridTrackEntry, StyleValueData};

unsafe extern "C" {
    /// Interns the concatenation of a live fly string and an ASCII suffix,
    /// returning one leaked reference to the result.
    fn ladybird_utf16_fly_string_concat_ascii(raw: usize, suffix: *const u8, suffix_length: usize) -> usize;
}

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
    /// An opaque C++ context the registered payload assemblers receive
    /// through their assembly structs, for the members only C++ can build:
    /// stamped image wrapper mints and the color fallback arm.
    pub cpp_assembler_context: *const c_void,
}

/// The longhand table joined with the effective-value overrides: exactly the
/// values `ComputedProperties::property()` returns during a group build.
struct EffectiveValues<'a> {
    table: &'a ComputedLonghandTable,
    override_properties: &'a [u16],
    override_values: &'a [*const c_void],
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
) -> Option<Vec<FfiGroupValueEntry>> {
    let all_descriptors = registered_field_descriptors()?;
    let mut entries: Vec<FfiGroupValueEntry> = Vec::new();
    for descriptor in all_descriptors
        .iter()
        .filter(|descriptor| descriptor.group_index as usize == group_index)
    {
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
    let position_anchor_name_leaked_raw = match values.value(property_id::POSITION_ANCHOR) {
        Some(StyleValueData::CustomIdent { custom_ident }) => {
            let retained = custom_ident.clone();
            let raw = retained.raw();
            // The surround builder assumes the leaked reference.
            std::mem::forget(retained);
            raw
        }
        _ => 0,
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
            position_anchor_name_leaked_raw,
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

    /// Interns the implicit `{base}{suffix}` grid line name.
    fn intern_implicit(&mut self, base: &RetainedUtf16FlyString, suffix: &str) -> u32 {
        // SAFETY: The base names a live fly string and the suffix is ASCII;
        // the bridge returns one leaked reference the intern assumes.
        let name = unsafe {
            RetainedUtf16FlyString::from_leaked_raw(ladybird_utf16_fly_string_concat_ascii(
                base.raw(),
                suffix.as_ptr(),
                suffix.len(),
            ))
        };
        self.intern_retained(name)
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
                result.implicit_start_name_index = arena.intern_implicit(name, "-start");
                result.implicit_end_name_index = arena.intern_implicit(name, "-end");
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
        let implicit_start_name_index = arena.intern_implicit(area.name(), "-start");
        let implicit_end_name_index = arena.intern_implicit(area.name(), "-end");
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

/// One pre-lowered transform function: a baked row-major matrix, or a
/// translate whose percentage-bearing axes keep their per-axis value slots
/// for the reference box, mirroring CSS::ResolvedTransform.
#[repr(C)]
pub struct FfiResolvedTransformEntry {
    pub is_translate: bool,
    /// Row-major FloatMatrix4x4 constructor elements.
    pub matrix: [f32; 16],
    pub x_px: f32,
    pub y_px: f32,
    pub z_px: f32,
    /// Retained percentage-bearing axis values, null for plain axes; the
    /// assembler assumes the references.
    pub x_percentage: *const c_void,
    pub y_percentage: *const c_void,
}

/// The transform group's complex members, pre-lowered for the registered C++
/// assembler. Every pointer is a retained style value reference the
/// assembler assumes.
#[repr(C)]
pub struct FfiTransformGroupAssembly {
    /// The transform property's value list, null when the computed transform
    /// is none.
    pub transform_list: *const c_void,
    pub rotate: *const c_void,
    pub translate: *const c_void,
    pub scale: *const c_void,
    /// The paint-ready lowering of translate, rotate, scale, and the
    /// transform functions, in that order.
    pub resolved_transforms: *const FfiResolvedTransformEntry,
    pub resolved_transform_count: usize,
    /// The transform-origin axis values, null when the computed value is not
    /// the three-value list form.
    pub transform_origin_x: *const c_void,
    pub transform_origin_y: *const c_void,
    pub transform_origin_z: *const c_void,
    pub has_perspective: bool,
    /// Raw CSSPixels.
    pub perspective_px: i32,
    /// The perspective-origin position value.
    pub perspective_origin: *const c_void,
}

/// One lowered filter operation, mirroring CSS::Filter's plain operations.
/// The kind codes are the C++ FilterStyleValue::Kind values (blur 0,
/// drop-shadow 1, hue-rotate 2, color 3) plus 4 for a url() reference.
#[repr(C)]
pub struct FfiLoweredFilterOperation {
    pub kind: u8,
    /// The Gfx::ColorFilterType code for color operations.
    pub color_operation: u8,
    /// Blur radius in px, color-operation amount, or hue-rotate degrees.
    pub amount: f32,
    /// Drop-shadow geometry as raw CSSPixels and its resolved color.
    pub shadow_offset_x: i32,
    pub shadow_offset_y: i32,
    pub shadow_radius: i32,
    pub shadow_color: u32,
    /// The retained url style value for kind 4; the assembler assumes the
    /// reference and extracts the fragment.
    pub url_value: *const c_void,
}

/// One filter property's lowering: the retained value list (null for none)
/// and the pre-lowered operations.
#[repr(C)]
pub struct FfiLoweredFilter {
    pub filter_list: *const c_void,
    pub operations: *const FfiLoweredFilterOperation,
    pub operation_count: usize,
}

/// One lowered box-shadow layer, mirroring CSS::ShadowData.
#[repr(C)]
pub struct FfiLoweredShadow {
    /// Raw CSSPixels.
    pub offset_x: i32,
    pub offset_y: i32,
    pub blur_radius: i32,
    pub spread_distance: i32,
    pub color: u32,
    /// The C++ ColorSyntax code: legacy 0, modern 1.
    pub color_syntax: u8,
    /// The C++ ShadowPlacement code.
    pub placement: u8,
}

/// One clip rect edge: auto, or a length in its original unit (calc resolves
/// to px).
#[repr(C)]
pub struct FfiLoweredClipEdge {
    pub is_auto: bool,
    pub value: f64,
    pub unit: u8,
}

/// The effects group's complex members, pre-lowered for the registered C++
/// assembler.
#[repr(C)]
pub struct FfiEffectsGroupAssembly {
    pub filter: FfiLoweredFilter,
    pub backdrop_filter: FfiLoweredFilter,
    pub box_shadows: *const FfiLoweredShadow,
    pub box_shadow_count: usize,
    pub clip_is_rect: bool,
    /// Top, right, bottom, left.
    pub clip_edges: [FfiLoweredClipEdge; 4],
}

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

fn baked_matrix_entry(matrix: [f32; 16]) -> FfiResolvedTransformEntry {
    FfiResolvedTransformEntry {
        is_translate: false,
        matrix,
        x_px: 0.0,
        y_px: 0.0,
        z_px: 0.0,
        x_percentage: std::ptr::null(),
        y_percentage: std::ptr::null(),
    }
}

/// The TransformationStyleValue::to_resolved_transform port: only the
/// translate family takes length-percentages, so a percentage-bearing
/// translate keeps per-axis slots and everything else bakes into a matrix.
fn lower_transformation(data: &StyleValueData) -> FfiResolvedTransformEntry {
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
    let lower_axis = |index: usize| -> (f32, *const c_void) {
        if index >= values.len() {
            return (0.0, std::ptr::null());
        }
        let value = &values[index];
        if axis_needs_reference_box(index) {
            // SAFETY: The axis value is live table data; the assembler
            // assumes the retained reference.
            let retained = unsafe { crate::css::style_value::rust_style_value_retain(value.pointer()) };
            return (0.0, retained.cast());
        }
        (length_to_css_pixels(value.data()).to_float(), std::ptr::null())
    };
    let translate_entry = |x: (f32, *const c_void), y: (f32, *const c_void), z: f32| FfiResolvedTransformEntry {
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
                return translate_entry(lower_axis(0), (0.0, std::ptr::null()), 0.0);
            }
        }
        functions::TRANSLATE_Y if values.len() == 1 && axis_needs_reference_box(0) => {
            return translate_entry((0.0, std::ptr::null()), lower_axis(0), 0.0);
        }
        _ => {}
    }

    baked_matrix_entry(transformation_to_matrix(function, values))
}

/// Builds the transform group: matrices and resolved lengths lower natively,
/// and the wrapper-backed members travel to the registered C++ assembler as
/// retained handles.
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

    let retain = |pointer: *const c_void| -> *const c_void {
        // SAFETY: The pointer names live table data; the assembler assumes
        // the retained reference.
        unsafe { crate::css::style_value::rust_style_value_retain(pointer.cast()) }.cast()
    };

    let mut resolved: Vec<FfiResolvedTransformEntry> = Vec::new();
    // Pre-lower in the order the transformation matrix accumulates them:
    // translate, rotate, scale, then the transform property's functions.
    let mut individual = |property: u16| -> *const c_void {
        match values.value(property) {
            Some(data @ StyleValueData::Transformation { .. }) => {
                resolved.push(lower_transformation(data));
                retain(values.pointer(property))
            }
            _ => std::ptr::null(),
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
            retain(values.pointer(property_id::TRANSFORM))
        }
        _ => std::ptr::null(),
    };

    let (transform_origin_x, transform_origin_y, transform_origin_z) = match values.value(property_id::TRANSFORM_ORIGIN)
    {
        Some(StyleValueData::ValueList { values: list, .. }) if list.as_slice().len() == 3 => {
            let axes = list.as_slice();
            (
                retain(axes[0].pointer().cast()),
                retain(axes[1].pointer().cast()),
                retain(axes[2].pointer().cast()),
            )
        }
        _ => (std::ptr::null(), std::ptr::null(), std::ptr::null()),
    };

    let (has_perspective, perspective_px) = match values.value(property_id::PERSPECTIVE) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => (false, 0),
        Some(data) => (true, length_to_css_pixels(data).raw_value()),
        None => (false, 0),
    };

    let assembly = FfiTransformGroupAssembly {
        transform_list,
        rotate,
        translate,
        scale,
        resolved_transforms: resolved.as_ptr(),
        resolved_transform_count: resolved.len(),
        transform_origin_x,
        transform_origin_y,
        transform_origin_z,
        has_perspective,
        perspective_px,
        perspective_origin: retain(values.pointer(property_id::PERSPECTIVE_ORIGIN)),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::TRANSFORM,
            &entries,
            (&raw const assembly).cast(),
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
) -> (Vec<FfiLoweredFilterOperation>, *const c_void) {
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
        return (Vec::new(), std::ptr::null());
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
        return (Vec::new(), std::ptr::null());
    }

    let empty_operation = || FfiLoweredFilterOperation {
        kind: 0,
        color_operation: 0,
        amount: 0.0,
        shadow_offset_x: 0,
        shadow_offset_y: 0,
        shadow_radius: 0,
        shadow_color: 0,
        url_value: std::ptr::null(),
    };
    let mut operations = Vec::with_capacity(list.len());
    for value in list {
        match value.data() {
            StyleValueData::Url { .. } => {
                // SAFETY: The list element is live table data; the assembler
                // assumes the retained reference.
                let retained = unsafe { crate::css::style_value::rust_style_value_retain(value.pointer()) };
                operations.push(FfiLoweredFilterOperation {
                    kind: FILTER_KIND_URL,
                    url_value: retained.cast(),
                    ..empty_operation()
                });
            }
            StyleValueData::Filter {
                kind,
                color_operation,
                value: filter_value,
            } => match *kind {
                FILTER_KIND_BLUR => {
                    operations.push(FfiLoweredFilterOperation {
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
                    operations.push(FfiLoweredFilterOperation {
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
                    operations.push(FfiLoweredFilterOperation {
                        kind: FILTER_KIND_HUE_ROTATE,
                        amount: angle_degrees(filter_value.data()) as f32,
                        ..empty_operation()
                    });
                }
                FILTER_KIND_COLOR => {
                    operations.push(FfiLoweredFilterOperation {
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
    // SAFETY: The list value is live table data; the assembler assumes the
    // retained reference.
    let retained_list = unsafe { crate::css::style_value::rust_style_value_retain(values.pointer(property).cast()) };
    (operations, retained_list.cast())
}

fn lower_shadow_layers(values: &EffectiveValues, property: u16, input: &ColorResolutionInput) -> Vec<FfiLoweredShadow> {
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
        shadows.push(FfiLoweredShadow {
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
            placement: *placement,
        });
    }
    shadows
}

fn lower_clip_edge(data: &StyleValueData) -> FfiLoweredClipEdge {
    match data {
        StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO => FfiLoweredClipEdge {
            is_auto: true,
            value: 0.0,
            unit: crate::css::style_compute::px_length_unit(),
        },
        StyleValueData::Length { value, unit } => FfiLoweredClipEdge {
            is_auto: false,
            value: *value,
            unit: *unit,
        },
        StyleValueData::Calculated { .. } => FfiLoweredClipEdge {
            is_auto: false,
            value: crate::css::calc::resolve_calculated_length_without_context(data, 0.0)
                .expect("a computed clip edge resolves without context"),
            unit: crate::css::style_compute::px_length_unit(),
        },
        _ => unreachable!("a computed clip edge is auto, a length or a calculation"),
    }
}

/// Builds the effects group: the filter operations, shadows and clip rect
/// lower natively, and the wrapper-backed members travel to the registered
/// C++ assembler as retained handles.
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

    let auto_edge = || FfiLoweredClipEdge {
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

    let assembly = FfiEffectsGroupAssembly {
        filter: FfiLoweredFilter {
            filter_list,
            operations: filter_operations.as_ptr(),
            operation_count: filter_operations.len(),
        },
        backdrop_filter: FfiLoweredFilter {
            filter_list: backdrop_list,
            operations: backdrop_operations.as_ptr(),
            operation_count: backdrop_operations.len(),
        },
        box_shadows: shadows.as_ptr(),
        box_shadow_count: shadows.len(),
        clip_is_rect,
        clip_edges,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::EFFECTS,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

// --- Background, mask and border lowering ----------------------------------
//
// The coordinated layer walks the C++ extractors ran over minted wrappers -
// background layers, mask layers, the border sides and border-image - lower
// here from the table slots. Enum-mapped keywords become their C++ enum codes
// through the generated converters; the genuinely C++ members (image wrapper
// RefPtrs, LengthPercentage and Position slots) travel as retained handles,
// and an image-bearing slot is flagged so the assembler mints its wrapper
// through the stamped property() path.

/// Retains a table or override value pointer for an assembly slot; the
/// assembler assumes the reference.
fn retain_for_assembly(pointer: *const c_void) -> *const c_void {
    assert!(!pointer.is_null());
    // SAFETY: The pointer names live style value data.
    unsafe { crate::css::style_value::rust_style_value_retain(pointer.cast()) }.cast()
}

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

/// Whether a computed value is an image the C++ side wraps as an
/// AbstractImageStyleValue.
fn is_abstract_image(data: &StyleValueData) -> bool {
    matches!(
        data,
        StyleValueData::Image { .. }
            | StyleValueData::ImageSet { .. }
            | StyleValueData::LinearGradient { .. }
            | StyleValueData::ConicGradient { .. }
            | StyleValueData::RadialGradient { .. }
    )
}

/// Whether an image slot's wrapper must mint through the stamped property()
/// path: url() images and image sets read style sheet context (base URL,
/// origin cleanliness, pending image registration) when they load.
fn image_needs_stamped_wrapper(data: &StyleValueData) -> bool {
    matches!(data, StyleValueData::Image { .. } | StyleValueData::ImageSet { .. })
}

/// One lowered background or mask layer. Every enum field holds the C++ enum
/// code; the pointers are retained style values the assembler assumes, with
/// the image slot left null when its wrapper mints through property().
#[repr(C)]
pub struct FfiCoordinatedLayerAssembly {
    /// The layer's image slot value, retained; null when the wrapper mints
    /// through the stamped property() path instead.
    pub image: *const c_void,
    pub image_is_abstract_image: bool,
    pub image_needs_stamped_wrapper: bool,
    pub attachment: u8,
    pub blend_mode: u8,
    pub clip: u8,
    pub origin: u8,
    pub mask_clip_is_no_clip: bool,
    pub mask_clip: u8,
    pub mask_composite: u8,
    pub mask_mode: u8,
    pub mask_origin: u8,
    /// Retained offset values of the layer position's x and y edges.
    pub position_x: *const c_void,
    pub position_y: *const c_void,
    pub repeat_x: u8,
    pub repeat_y: u8,
    /// The C++ BackgroundSize code: contain 0, cover 1, length-percentage 2.
    pub size_type: u8,
    pub size_x: *const c_void,
    pub size_y: *const c_void,
}

impl FfiCoordinatedLayerAssembly {
    fn empty() -> Self {
        Self {
            image: std::ptr::null(),
            image_is_abstract_image: false,
            image_needs_stamped_wrapper: false,
            attachment: 0,
            blend_mode: 0,
            clip: 0,
            origin: 0,
            mask_clip_is_no_clip: false,
            mask_clip: 0,
            mask_composite: 0,
            mask_mode: 0,
            mask_origin: 0,
            position_x: std::ptr::null(),
            position_y: std::ptr::null(),
            repeat_x: 0,
            repeat_y: 0,
            size_type: 0,
            size_x: std::ptr::null(),
            size_y: std::ptr::null(),
        }
    }

    fn set_image(&mut self, data: &StyleValueData, pointer: *const c_void) {
        self.image_is_abstract_image = is_abstract_image(data);
        self.image_needs_stamped_wrapper = image_needs_stamped_wrapper(data);
        if !self.image_needs_stamped_wrapper {
            self.image = retain_for_assembly(pointer);
        }
    }

    fn set_position_offsets(&mut self, x_data: &StyleValueData, y_data: &StyleValueData) {
        let offset_of = |edge: &StyleValueData| -> *const c_void {
            let StyleValueData::Edge { offset, .. } = edge else {
                unreachable!("a computed layer position component is an edge value");
            };
            retain_for_assembly(offset.pointer().cast())
        };
        self.position_x = offset_of(x_data);
        self.position_y = offset_of(y_data);
    }

    fn set_repeat(&mut self, data: &StyleValueData) {
        let StyleValueData::RepeatStyle { repeat_x, repeat_y } = data else {
            unreachable!("a computed layer repeat is a repeat-style value");
        };
        self.repeat_x = *repeat_x;
        self.repeat_y = *repeat_y;
    }

    fn set_size(&mut self, data: &StyleValueData) {
        const BACKGROUND_SIZE_CONTAIN: u8 = 0;
        const BACKGROUND_SIZE_COVER: u8 = 1;
        const BACKGROUND_SIZE_LENGTH_PERCENTAGE: u8 = 2;
        match data {
            StyleValueData::BackgroundSize { size_x, size_y } => {
                self.size_type = BACKGROUND_SIZE_LENGTH_PERCENTAGE;
                self.size_x = retain_for_assembly(size_x.pointer().cast());
                self.size_y = retain_for_assembly(size_y.pointer().cast());
            }
            StyleValueData::Keyword { keyword: code } if *code == keyword::CONTAIN => {
                self.size_type = BACKGROUND_SIZE_CONTAIN;
            }
            StyleValueData::Keyword { keyword: code } if *code == keyword::COVER => {
                self.size_type = BACKGROUND_SIZE_COVER;
            }
            _ => unreachable!("a computed layer size is contain, cover or a background-size"),
        }
    }
}

/// The background group's complex members, pre-lowered for the registered
/// C++ assembler.
#[repr(C)]
pub struct FfiBackgroundGroupAssembly {
    pub cpp_context: *const c_void,
    pub layers: *const FfiCoordinatedLayerAssembly,
    pub layer_count: usize,
    /// Whether the core resolved background-color (and poked it); the
    /// assembler's C++ arm resolves it otherwise.
    pub color_resolved: bool,
    /// The final layer's background-clip as a C++ BackgroundBox code.
    pub color_clip: u8,
}

/// Builds the background group: the generic descriptor path first (an
/// all-initial or color-only background shares payloads exactly as before),
/// then the full layer lowering through the registered assembler.
unsafe fn build_background_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::BACKGROUND,
            values,
            input,
            used_color_scheme,
            parent_payload,
        )
    };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::BACKGROUND, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let item = |items: &[*const c_void], index: usize| -> &StyleValueData {
        // SAFETY: Repeatable item pointers name live table or override data.
        unsafe { &*items[index % items.len()].cast::<StyleValueData>() }
    };

    let image_items = repeatable_item_pointers(values, property_id::BACKGROUND_IMAGE);
    let attachment_items = repeatable_item_pointers(values, property_id::BACKGROUND_ATTACHMENT);
    let blend_mode_items = repeatable_item_pointers(values, property_id::BACKGROUND_BLEND_MODE);
    let clip_items = repeatable_item_pointers(values, property_id::BACKGROUND_CLIP);
    let origin_items = repeatable_item_pointers(values, property_id::BACKGROUND_ORIGIN);
    let position_x_items = repeatable_item_pointers(values, property_id::BACKGROUND_POSITION_X);
    let position_y_items = repeatable_item_pointers(values, property_id::BACKGROUND_POSITION_Y);
    let repeat_items = repeatable_item_pointers(values, property_id::BACKGROUND_REPEAT);
    let size_items = repeatable_item_pointers(values, property_id::BACKGROUND_SIZE);

    let keyword_code = |data: &StyleValueData, map: fn(u16) -> Option<u8>, what: &str| -> u8 {
        map(keyword_of(data).unwrap_or_else(|| unreachable!("a computed {what} is a keyword")))
            .unwrap_or_else(|| unreachable!("a computed {what} keyword maps to its enum"))
    };

    let mut layers = Vec::with_capacity(image_items.len());
    for index in 0..image_items.len() {
        let mut layer = FfiCoordinatedLayerAssembly::empty();
        layer.set_image(item(&image_items, index), image_items[index]);
        layer.attachment = keyword_code(
            item(&attachment_items, index),
            crate::css::css_enums::keyword_to_background_attachment,
            "background-attachment",
        );
        layer.blend_mode = keyword_code(
            item(&blend_mode_items, index),
            crate::css::css_enums::keyword_to_mix_blend_mode,
            "background-blend-mode",
        );
        layer.clip = keyword_code(
            item(&clip_items, index),
            crate::css::css_enums::keyword_to_background_box,
            "background-clip",
        );
        layer.origin = keyword_code(
            item(&origin_items, index),
            crate::css::css_enums::keyword_to_background_box,
            "background-origin",
        );
        layer.set_position_offsets(item(&position_x_items, index), item(&position_y_items, index));
        layer.set_repeat(item(&repeat_items, index));
        layer.set_size(item(&size_items, index));
        layers.push(layer);
    }

    // The background color is clipped by the final layer's background-clip
    // value, coordinated against the image count.
    let color_clip = keyword_code(
        item(&clip_items, image_items.len() - 1),
        crate::css::css_enums::keyword_to_background_box,
        "background-clip",
    );
    let color_resolved = values
        .value(property_id::BACKGROUND_COLOR)
        .is_some_and(|data| resolved_color(input, property_id::BACKGROUND_COLOR, data).is_some());

    let assembly = FfiBackgroundGroupAssembly {
        cpp_context: cpp_assembler_context,
        layers: layers.as_ptr(),
        layer_count: layers.len(),
        color_resolved,
        color_clip,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::BACKGROUND,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// One lowered mask position, as the retained offset values of its x and y
/// edges.
#[repr(C)]
pub struct FfiPositionAssembly {
    pub x_offset: *const c_void,
    pub y_offset: *const c_void,
}

/// The mask group's complex members, pre-lowered for the registered C++
/// assembler.
#[repr(C)]
pub struct FfiMaskGroupAssembly {
    pub cpp_context: *const c_void,
    pub layers: *const FfiCoordinatedLayerAssembly,
    pub layer_count: usize,
    pub positions: *const FfiPositionAssembly,
    pub position_count: usize,
    /// The first mask-image value when it is a url() reference, retained.
    pub mask_reference_url: *const c_void,
    /// Whether the first mask-image value is an abstract image, whose layer
    /// wrapper doubles as the group's mask-image member.
    pub first_image_is_abstract_image: bool,
    /// 0 none, 1 url (retained), 2 basic shape (retained).
    pub clip_path_kind: u8,
    pub clip_path: *const c_void,
}

/// Builds the mask group: the generic descriptor path first, then the full
/// layer lowering through the registered assembler.
unsafe fn build_mask_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe { build_generic_group(group_index::MASK, values, input, used_color_scheme, parent_payload) };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::MASK, values, input, used_color_scheme) }) else {
        return std::ptr::null();
    };

    let item = |items: &[*const c_void], index: usize| -> &StyleValueData {
        // SAFETY: Repeatable item pointers name live table or override data.
        unsafe { &*items[index % items.len()].cast::<StyleValueData>() }
    };

    let image_items = repeatable_item_pointers(values, property_id::MASK_IMAGE);
    let clip_items = repeatable_item_pointers(values, property_id::MASK_CLIP);
    let composite_items = repeatable_item_pointers(values, property_id::MASK_COMPOSITE);
    let mode_items = repeatable_item_pointers(values, property_id::MASK_MODE);
    let origin_items = repeatable_item_pointers(values, property_id::MASK_ORIGIN);
    let position_items = repeatable_item_pointers(values, property_id::MASK_POSITION);
    let repeat_items = repeatable_item_pointers(values, property_id::MASK_REPEAT);
    let size_items = repeatable_item_pointers(values, property_id::MASK_SIZE);

    let keyword_code = |data: &StyleValueData, map: fn(u16) -> Option<u8>, what: &str| -> u8 {
        map(keyword_of(data).unwrap_or_else(|| unreachable!("a computed {what} is a keyword")))
            .unwrap_or_else(|| unreachable!("a computed {what} keyword maps to its enum"))
    };
    // The C++ BackgroundBox::BorderBox code the extractor defaulted the
    // layer's origin and clip to.
    let border_box = crate::css::css_enums::keyword_to_background_box(keyword::BORDER_BOX)
        .expect("border-box maps to a background box");

    let mut layers = Vec::with_capacity(image_items.len());
    for index in 0..image_items.len() {
        let mut layer = FfiCoordinatedLayerAssembly::empty();
        layer.origin = border_box;
        layer.clip = border_box;
        layer.set_image(item(&image_items, index), image_items[index]);

        let clip_data = item(&clip_items, index);
        let clip_keyword = keyword_of(clip_data).unwrap_or_else(|| unreachable!("a computed mask-clip is a keyword"));
        if clip_keyword != keyword::NO_CLIP {
            layer.mask_clip = keyword_code(clip_data, crate::css::css_enums::keyword_to_coord_box, "mask-clip");
            if let Some(clip) = crate::css::css_enums::keyword_to_background_box(clip_keyword) {
                layer.clip = clip;
            }
        } else {
            layer.mask_clip_is_no_clip = true;
        }

        layer.mask_composite = keyword_code(
            item(&composite_items, index),
            crate::css::css_enums::keyword_to_compositing_operator,
            "mask-composite",
        );
        layer.mask_mode = keyword_code(
            item(&mode_items, index),
            crate::css::css_enums::keyword_to_masking_mode,
            "mask-mode",
        );

        let origin_data = item(&origin_items, index);
        layer.mask_origin = keyword_code(origin_data, crate::css::css_enums::keyword_to_coord_box, "mask-origin");
        if let Some(origin) = keyword_of(origin_data).and_then(crate::css::css_enums::keyword_to_background_box) {
            layer.origin = origin;
        }

        let StyleValueData::Position { edge_x, edge_y } = item(&position_items, index) else {
            unreachable!("a computed mask-position is a position value");
        };
        layer.set_position_offsets(edge_x.data(), edge_y.data());
        layer.set_repeat(item(&repeat_items, index));
        layer.set_size(item(&size_items, index));
        layers.push(layer);
    }

    // The group's mask positions follow the mask-position list itself, not
    // the coordinated layer count.
    let positions: Vec<FfiPositionAssembly> = position_items
        .iter()
        .map(|pointer| {
            // SAFETY: Repeatable item pointers name live data.
            let StyleValueData::Position { edge_x, edge_y } = (unsafe { &*pointer.cast::<StyleValueData>() }) else {
                unreachable!("a computed mask-position is a position value");
            };
            let offset_of = |edge: &crate::css::style_value::RetainedStyleValueData| -> *const c_void {
                let StyleValueData::Edge { offset, .. } = edge.data() else {
                    unreachable!("a computed position component is an edge value");
                };
                retain_for_assembly(offset.pointer().cast())
            };
            FfiPositionAssembly {
                x_offset: offset_of(edge_x),
                y_offset: offset_of(edge_y),
            }
        })
        .collect();

    let first_image = item(&image_items, 0);
    let mask_reference_url = match first_image {
        StyleValueData::Url { .. } => retain_for_assembly(image_items[0]),
        _ => std::ptr::null(),
    };

    let (clip_path_kind, clip_path) = match values.value(property_id::CLIP_PATH) {
        Some(data @ StyleValueData::Url { .. }) => (1u8, retain_for_assembly(std::ptr::from_ref(data).cast())),
        Some(data @ StyleValueData::BasicShape { .. }) => (2u8, retain_for_assembly(std::ptr::from_ref(data).cast())),
        _ => (0u8, std::ptr::null()),
    };

    let assembly = FfiMaskGroupAssembly {
        cpp_context: cpp_assembler_context,
        layers: layers.as_ptr(),
        layer_count: layers.len(),
        positions: positions.as_ptr(),
        position_count: positions.len(),
        mask_reference_url,
        first_image_is_abstract_image: is_abstract_image(first_image),
        clip_path_kind,
        clip_path,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::MASK,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// One lowered border side: the line style code and whether the core
/// resolved (and poked) the side's color.
#[repr(C)]
pub struct FfiBorderSideAssembly {
    pub line_style: u8,
    pub color_resolved: bool,
}

/// One lowered border-image slice, width or outset component.
#[repr(C)]
pub struct FfiBorderImageSlotAssembly {
    /// 0 number, 1 retained value (the assembler converts), 2 auto.
    pub kind: u8,
    pub number: f64,
    pub value: *const c_void,
}

/// The border group's complex members, pre-lowered for the registered C++
/// assembler. The sides' computed widths and colors poke through the field
/// descriptors; the radii travel as retained border-radius values.
#[repr(C)]
pub struct FfiBorderGroupAssembly {
    pub cpp_context: *const c_void,
    pub left: FfiBorderSideAssembly,
    pub top: FfiBorderSideAssembly,
    pub right: FfiBorderSideAssembly,
    pub bottom: FfiBorderSideAssembly,
    /// Bottom-left, bottom-right, top-left, top-right retained border-radius
    /// values.
    pub radii: [*const c_void; 4],
    /// The border-image-source value, retained; null when none or when the
    /// wrapper mints through the stamped property() path.
    pub border_image_source: *const c_void,
    pub border_image_source_is_abstract_image: bool,
    pub border_image_source_needs_stamped_wrapper: bool,
    /// Top, right, bottom, left.
    pub slice: [FfiBorderImageSlotAssembly; 4],
    pub slice_fill: bool,
    pub width: [FfiBorderImageSlotAssembly; 4],
    pub width_value_count: u8,
    pub outset: [FfiBorderImageSlotAssembly; 4],
    pub outset_value_count: u8,
    /// C++ BorderImageRepeat codes.
    pub border_image_repeat_x: u8,
    pub border_image_repeat_y: u8,
}

/// A number, or a calculation that resolves to one without context; the C++
/// number_from_style_value arm the border-image lowering used.
fn plain_number(data: &StyleValueData) -> Option<f64> {
    match data {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_number_without_context(data),
        _ => None,
    }
}

/// Builds the border group: the generic descriptor path first (all-none
/// borders share payloads exactly as before), then the styled-border and
/// border-image lowering through the registered assembler.
unsafe fn build_border_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe { build_generic_group(group_index::BORDER, values, input, used_color_scheme, parent_payload) };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::BORDER, values, input, used_color_scheme) }) else {
        return std::ptr::null();
    };

    let side = |style_property: u16, color_property: u16| -> FfiBorderSideAssembly {
        let style_keyword = values
            .value(style_property)
            .and_then(keyword_of)
            .unwrap_or_else(|| unreachable!("a computed border style is a keyword"));
        FfiBorderSideAssembly {
            line_style: crate::css::css_enums::keyword_to_line_style(style_keyword)
                .unwrap_or_else(|| unreachable!("a computed border style maps to a line style")),
            color_resolved: values
                .value(color_property)
                .is_some_and(|data| resolved_color(input, color_property, data).is_some()),
        }
    };

    let radius = |property: u16| -> *const c_void {
        assert!(
            matches!(values.value(property), Some(StyleValueData::BorderRadius { .. })),
            "a computed border radius is a border-radius value"
        );
        retain_for_assembly(values.pointer(property))
    };

    // border-image-source.
    let source_data = values
        .value(property_id::BORDER_IMAGE_SOURCE)
        .expect("the table holds border-image-source");
    let source_is_image = is_abstract_image(source_data);
    let source_needs_stamp = source_is_image && image_needs_stamped_wrapper(source_data);
    let border_image_source = if source_is_image && !source_needs_stamp {
        retain_for_assembly(values.pointer(property_id::BORDER_IMAGE_SOURCE))
    } else {
        std::ptr::null()
    };

    // border-image-slice.
    let Some(StyleValueData::BorderImageSlice {
        top,
        right,
        bottom,
        left,
        fill,
    }) = values.value(property_id::BORDER_IMAGE_SLICE)
    else {
        unreachable!("a computed border-image-slice is a border-image-slice value");
    };
    const SLOT_NUMBER: u8 = 0;
    const SLOT_VALUE: u8 = 1;
    const SLOT_AUTO: u8 = 2;
    let value_slot = |data: &StyleValueData, pointer: *const c_void| -> FfiBorderImageSlotAssembly {
        if let Some(number) = plain_number(data) {
            return FfiBorderImageSlotAssembly {
                kind: SLOT_NUMBER,
                number,
                value: std::ptr::null(),
            };
        }
        if matches!(data, StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO) {
            return FfiBorderImageSlotAssembly {
                kind: SLOT_AUTO,
                number: 0.0,
                value: std::ptr::null(),
            };
        }
        FfiBorderImageSlotAssembly {
            kind: SLOT_VALUE,
            number: 0.0,
            value: retain_for_assembly(pointer),
        }
    };
    // A calculated slice keeps its calculation value, matching the C++
    // BorderImageSliceValue variant; only a plain number lowers to one.
    let slice_slot = |slot: &crate::css::style_value::RetainedStyleValueData| -> FfiBorderImageSlotAssembly {
        if let StyleValueData::Number { value } = slot.data() {
            return FfiBorderImageSlotAssembly {
                kind: SLOT_NUMBER,
                number: *value,
                value: std::ptr::null(),
            };
        }
        FfiBorderImageSlotAssembly {
            kind: SLOT_VALUE,
            number: 0.0,
            value: retain_for_assembly(slot.pointer().cast()),
        }
    };
    let slice = [slice_slot(top), slice_slot(right), slice_slot(bottom), slice_slot(left)];

    // border-image-width and border-image-outset expand a one-to-four item
    // list over the four sides, top right bottom left, with wraparound.
    let expand_sides = |property: u16| -> ([FfiBorderImageSlotAssembly; 4], u8) {
        let items = repeatable_item_pointers(values, property);
        let slot = |index: usize| -> FfiBorderImageSlotAssembly {
            let pointer = items[index % items.len()];
            // SAFETY: Repeatable item pointers name live data.
            value_slot(unsafe { &*pointer.cast::<StyleValueData>() }, pointer)
        };
        let count = match values.value(property) {
            Some(StyleValueData::ValueList { values: list, .. }) => list.as_slice().len() as u8,
            _ => 1,
        };
        ([slot(0), slot(1), slot(2), slot(3)], count)
    };
    let (width, width_value_count) = expand_sides(property_id::BORDER_IMAGE_WIDTH);
    let (outset, outset_value_count) = expand_sides(property_id::BORDER_IMAGE_OUTSET);

    // border-image-repeat: one or two keywords, stretch when unmappable.
    let repeat_items = repeatable_item_pointers(values, property_id::BORDER_IMAGE_REPEAT);
    let repeat_at = |index: usize| -> u8 {
        // SAFETY: Repeatable item pointers name live data.
        let data = unsafe { &*repeat_items[index % repeat_items.len()].cast::<StyleValueData>() };
        keyword_of(data)
            .and_then(crate::css::css_enums::keyword_to_border_image_repeat)
            .unwrap_or(crate::css::css_enums::border_image_repeat::STRETCH)
    };

    let assembly = FfiBorderGroupAssembly {
        cpp_context: cpp_assembler_context,
        left: side(property_id::BORDER_LEFT_STYLE, property_id::BORDER_LEFT_COLOR),
        top: side(property_id::BORDER_TOP_STYLE, property_id::BORDER_TOP_COLOR),
        right: side(property_id::BORDER_RIGHT_STYLE, property_id::BORDER_RIGHT_COLOR),
        bottom: side(property_id::BORDER_BOTTOM_STYLE, property_id::BORDER_BOTTOM_COLOR),
        radii: [
            radius(property_id::BORDER_BOTTOM_LEFT_RADIUS),
            radius(property_id::BORDER_BOTTOM_RIGHT_RADIUS),
            radius(property_id::BORDER_TOP_LEFT_RADIUS),
            radius(property_id::BORDER_TOP_RIGHT_RADIUS),
        ],
        border_image_source,
        border_image_source_is_abstract_image: source_is_image,
        border_image_source_needs_stamped_wrapper: source_needs_stamp,
        slice,
        slice_fill: *fill,
        width,
        width_value_count,
        outset,
        outset_value_count,
        border_image_repeat_x: repeat_at(0),
        border_image_repeat_y: repeat_at(1),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::BORDER,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

// --- SVG, list and content lowering -----------------------------------------

/// One lowered SVG paint (fill or stroke).
#[repr(C)]
pub struct FfiSvgPaintAssembly {
    /// 0 none, 1 lowered, 2 whole-value C++ resolution arm.
    pub kind: u8,
    pub is_url: bool,
    /// The retained url value for a url paint.
    pub url: *const c_void,
    pub has_color: bool,
    pub color: u32,
    pub color_is_currentcolor: bool,
    /// The whole property value, retained, for kind 2.
    pub value: *const c_void,
}

const SVG_PAINT_NONE: u8 = 0;
const SVG_PAINT_LOWERED: u8 = 1;
const SVG_PAINT_CPP: u8 = 2;

fn lower_svg_paint(values: &EffectiveValues, property: u16, input: &ColorResolutionInput) -> FfiSvgPaintAssembly {
    let mut paint = FfiSvgPaintAssembly {
        kind: SVG_PAINT_CPP,
        is_url: false,
        url: std::ptr::null(),
        has_color: false,
        color: 0,
        color_is_currentcolor: false,
        value: std::ptr::null(),
    };
    let data = values.value(property).expect("the table holds the paint property");
    match data {
        StyleValueData::Keyword { keyword: code } if *code == keyword::NONE => {
            paint.kind = SVG_PAINT_NONE;
            return paint;
        }
        StyleValueData::ValueList { values: list, .. } if list.as_slice().len() == 2 => {
            let components = list.as_slice();
            paint.is_url = true;
            match components[1].data() {
                StyleValueData::EmptyOptional => {
                    paint.kind = SVG_PAINT_LOWERED;
                    paint.url = retain_for_assembly(components[0].pointer().cast());
                    return paint;
                }
                fallback => {
                    if let Some(color) = to_color(fallback, input) {
                        paint.kind = SVG_PAINT_LOWERED;
                        paint.url = retain_for_assembly(components[0].pointer().cast());
                        paint.has_color = true;
                        paint.color = packed_color(color);
                        paint.color_is_currentcolor = matches!(fallback, StyleValueData::Keyword { keyword: code } if *code == keyword::CURRENTCOLOR);
                        return paint;
                    }
                }
            }
        }
        _ => {
            if let Some(color) = to_color(data, input) {
                paint.kind = SVG_PAINT_LOWERED;
                paint.has_color = true;
                paint.color = packed_color(color);
                paint.color_is_currentcolor =
                    matches!(data, StyleValueData::Keyword { keyword: code } if *code == keyword::CURRENTCOLOR);
                return paint;
            }
        }
    }
    paint.value = retain_for_assembly(values.pointer(property));
    paint
}

/// One lowered stroke-dasharray item: a plain number, or a retained
/// length-percentage value.
#[repr(C)]
pub struct FfiDashItemAssembly {
    pub is_number: bool,
    pub number: f64,
    pub value: *const c_void,
}

/// A lowered length-percentage-or-number slot: SVG stroke widths and dash
/// offsets treat plain numbers as user-unit pixels.
#[repr(C)]
pub struct FfiLengthPercentageOrNumberAssembly {
    pub is_number: bool,
    pub number: f64,
    pub value: *const c_void,
}

/// Whether a slot holds a plain pixel length, exactly the form the
/// CSS_PIXELS descriptor pokes.
fn is_px_length(values: &EffectiveValues, property: u16) -> bool {
    matches!(
        values.value(property),
        Some(StyleValueData::Length { unit, .. }) if *unit == crate::css::style_compute::px_length_unit()
    )
}

fn lower_length_percentage_or_number(values: &EffectiveValues, property: u16) -> FfiLengthPercentageOrNumberAssembly {
    let data = values.value(property).expect("the table holds the property");
    let number = match data {
        StyleValueData::Number { value } => Some(*value),
        data @ StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_number_without_context(data),
        _ => None,
    };
    match number {
        Some(number) => FfiLengthPercentageOrNumberAssembly {
            is_number: true,
            number,
            value: std::ptr::null(),
        },
        None => FfiLengthPercentageOrNumberAssembly {
            is_number: false,
            number: 0.0,
            value: retain_for_assembly(values.pointer(property)),
        },
    }
}

/// The inherited SVG group's complex members, pre-lowered for the registered
/// C++ assembler.
#[repr(C)]
pub struct FfiInheritedSvgGroupAssembly {
    pub cpp_context: *const c_void,
    pub fill: FfiSvgPaintAssembly,
    pub stroke: FfiSvgPaintAssembly,
    pub dashes: *const FfiDashItemAssembly,
    pub dash_count: usize,
    pub stroke_dashoffset: FfiLengthPercentageOrNumberAssembly,
    pub stroke_width: FfiLengthPercentageOrNumberAssembly,
    /// C++ PaintOrder codes, first to last.
    pub paint_order: [u8; 3],
    pub paint_order_serialization_length: u8,
    pub paint_order_is_normal: bool,
    pub has_dominant_baseline: bool,
    pub dominant_baseline: u8,
}

/// Builds the inherited SVG group: the generic descriptor path first, then
/// the paint, dash and paint-order lowering through the registered assembler.
unsafe fn build_inherited_svg_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::INHERITED_SVG,
            values,
            input,
            used_color_scheme,
            parent_payload,
        )
    };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::INHERITED_SVG, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    // stroke-dasharray: none is the empty list; everything else is a list of
    // lengths, percentages, numbers and calculations.
    let mut dashes: Vec<FfiDashItemAssembly> = Vec::new();
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
                Some(number) => FfiDashItemAssembly {
                    is_number: true,
                    number,
                    value: std::ptr::null(),
                },
                None => FfiDashItemAssembly {
                    is_number: false,
                    number: 0.0,
                    value: retain_for_assembly(item.pointer().cast()),
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

    let assembly = FfiInheritedSvgGroupAssembly {
        cpp_context: cpp_assembler_context,
        fill: lower_svg_paint(values, property_id::FILL, input),
        stroke: lower_svg_paint(values, property_id::STROKE, input),
        dashes: dashes.as_ptr(),
        dash_count: dashes.len(),
        stroke_dashoffset: lower_length_percentage_or_number(values, property_id::STROKE_DASHOFFSET),
        stroke_width: lower_length_percentage_or_number(values, property_id::STROKE_WIDTH),
        paint_order: order,
        paint_order_serialization_length: serialization_length,
        paint_order_is_normal: is_normal,
        has_dominant_baseline: dominant_baseline.is_some(),
        dominant_baseline: dominant_baseline.unwrap_or(0),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::INHERITED_SVG,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// The inherited list group's complex members, pre-lowered for the
/// registered C++ assembler. The list-style-type stays with the assembler's
/// C++ arm: its counter styles resolve against the style scope.
#[repr(C)]
pub struct FfiInheritedListGroupAssembly {
    pub cpp_context: *const c_void,
    /// 0 none, 1 image minted through the stamped property() path, 2 image
    /// adopted from the retained handle.
    pub image_kind: u8,
    pub image: *const c_void,
    /// 0 auto, 1 none, 2 specified pairs.
    pub quotes_kind: u8,
    /// Borrowed fly-string raws, in open/close pairs, alive across the build.
    pub quote_strings: *const usize,
    pub quote_string_count: usize,
}

/// Builds the inherited list group: the generic descriptor path first, then
/// the image and quotes lowering through the registered assembler.
unsafe fn build_inherited_list_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
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

    const LIST_IMAGE_NONE: u8 = 0;
    const LIST_IMAGE_STAMPED: u8 = 1;
    const LIST_IMAGE_RETAINED: u8 = 2;
    let image_data = values
        .value(property_id::LIST_STYLE_IMAGE)
        .expect("the table holds list-style-image");
    let (image_kind, image) = if !is_abstract_image(image_data) {
        (LIST_IMAGE_NONE, std::ptr::null())
    } else if image_needs_stamped_wrapper(image_data) {
        (LIST_IMAGE_STAMPED, std::ptr::null())
    } else {
        (
            LIST_IMAGE_RETAINED,
            retain_for_assembly(values.pointer(property_id::LIST_STYLE_IMAGE)),
        )
    };

    const QUOTES_AUTO: u8 = 0;
    const QUOTES_NONE: u8 = 1;
    const QUOTES_SPECIFIED: u8 = 2;
    let mut quotes_kind = QUOTES_AUTO;
    let mut quote_strings: Vec<usize> = Vec::new();
    match values.value(property_id::QUOTES) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => quotes_kind = QUOTES_NONE,
        Some(StyleValueData::ValueList { values: list, .. }) => {
            let items = list.as_slice();
            assert!(items.len() % 2 == 0, "computed quotes come in pairs");
            quotes_kind = QUOTES_SPECIFIED;
            for item in items {
                let StyleValueData::String { string } = item.data() else {
                    unreachable!("a computed quotes item is a string");
                };
                quote_strings.push(string.raw());
            }
        }
        // auto, and any other keyword the extractor folded to the initial value.
        _ => {}
    }

    let assembly = FfiInheritedListGroupAssembly {
        cpp_context: cpp_assembler_context,
        image_kind,
        image,
        quotes_kind,
        quote_strings: quote_strings.as_ptr(),
        quote_string_count: quote_strings.len(),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::INHERITED_LIST,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// One lowered counter definition.
#[repr(C)]
pub struct FfiCounterDataAssembly {
    /// A borrowed fly-string raw, alive across the build.
    pub name_raw: usize,
    pub is_reversed: bool,
    pub has_value: bool,
    pub value: i32,
}

/// The content group's complex members, pre-lowered for the registered C++
/// assembler. A content list stays with the assembler's C++ arm: its items
/// are wrapper-typed (strings, counters, images) and its images mint through
/// the stamped property() path.
#[repr(C)]
pub struct FfiContentGroupAssembly {
    pub cpp_context: *const c_void,
    /// 0 normal, 1 none, 2 list (the assembler walks the stamped wrapper).
    pub content_kind: u8,
    pub counter_increment: *const FfiCounterDataAssembly,
    pub counter_increment_count: usize,
    pub counter_reset: *const FfiCounterDataAssembly,
    pub counter_reset_count: usize,
    pub counter_set: *const FfiCounterDataAssembly,
    pub counter_set_count: usize,
}

fn lower_counter_data(values: &EffectiveValues, property: u16) -> Vec<FfiCounterDataAssembly> {
    let Some(StyleValueData::CounterDefinitions { counter_definitions }) = values.value(property) else {
        // The none keyword, and anything the extractor did not handle, is the
        // empty list.
        return Vec::new();
    };
    counter_definitions
        .as_slice()
        .iter()
        .map(|definition| {
            let value = definition
                .value()
                .optional_data()
                .map(|value| grid_integer(value).expect("a computed counter value resolves without context"));
            FfiCounterDataAssembly {
                name_raw: definition.name().raw(),
                is_reversed: definition.is_reversed(),
                has_value: value.is_some(),
                value: value.unwrap_or(0),
            }
        })
        .collect()
}

/// Builds the content group: the generic descriptor path first, then the
/// counter lowering (and the content list's C++ arm) through the registered
/// assembler.
unsafe fn build_content_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
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

    const CONTENT_NORMAL: u8 = 0;
    const CONTENT_NONE: u8 = 1;
    const CONTENT_LIST: u8 = 2;
    let content_kind = match values.value(property_id::CONTENT) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::NONE => CONTENT_NONE,
        Some(StyleValueData::Content { .. }) => CONTENT_LIST,
        _ => CONTENT_NORMAL,
    };

    let counter_increment = lower_counter_data(values, property_id::COUNTER_INCREMENT);
    let counter_reset = lower_counter_data(values, property_id::COUNTER_RESET);
    let counter_set = lower_counter_data(values, property_id::COUNTER_SET);

    let assembly = FfiContentGroupAssembly {
        cpp_context: cpp_assembler_context,
        content_kind,
        counter_increment: counter_increment.as_ptr(),
        counter_increment_count: counter_increment.len(),
        counter_reset: counter_reset.as_ptr(),
        counter_reset_count: counter_reset.len(),
        counter_set: counter_set.as_ptr(),
        counter_set_count: counter_set.len(),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::CONTENT,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

// --- Inherited UI, inherited text and misc lowering -------------------------

/// One lowered cursor list item: a predefined cursor code, or a retained
/// cursor() value whose image wrapper the assembler adopts.
#[repr(C)]
pub struct FfiCursorItemAssembly {
    pub is_cursor_value: bool,
    pub cursor: *const c_void,
    pub predefined: u8,
}

/// The inherited UI group's complex members, pre-lowered for the registered
/// C++ assembler.
#[repr(C)]
pub struct FfiInheritedUiGroupAssembly {
    pub cpp_context: *const c_void,
    pub caret_is_auto: bool,
    pub caret_resolved: bool,
    pub accent_is_auto: bool,
    pub accent_resolved: bool,
    pub cursors: *const FfiCursorItemAssembly,
    pub cursor_count: usize,
    /// 0 auto, 1 lowered colors, 2 C++ resolution arm.
    pub scrollbar_color_kind: u8,
    pub scrollbar_thumb_color: u32,
    pub scrollbar_track_color: u32,
    /// Borrowed fly-string raws of the color-scheme names, alive across the
    /// build.
    pub color_schemes: *const usize,
    pub color_scheme_count: usize,
    pub color_scheme_only: bool,
}

/// Builds the inherited UI group: the generic descriptor path first, then
/// the cursor, caret, accent and scrollbar-color lowering through the
/// registered assembler.
unsafe fn build_inherited_ui_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::INHERITED_UI,
            values,
            input,
            used_color_scheme,
            parent_payload,
        )
    };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::INHERITED_UI, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    let color_or_auto = |property: u16| -> (bool, bool) {
        let data = values.value(property).expect("the table holds the color property");
        let is_auto = matches!(data, StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO);
        let resolved = resolved_color(input, property, data).is_some();
        (is_auto, resolved)
    };
    let (caret_is_auto, caret_resolved) = color_or_auto(property_id::CARET_COLOR);
    let (accent_is_auto, accent_resolved) = color_or_auto(property_id::ACCENT_COLOR);

    // The cursor list, with the extractor's rules: unmappable keywords are
    // skipped, and an empty result is the predefined auto cursor.
    let mut cursors: Vec<FfiCursorItemAssembly> = Vec::new();
    let mut push_cursor = |data: &StyleValueData, pointer: *const c_void| match data {
        StyleValueData::Cursor { .. } => cursors.push(FfiCursorItemAssembly {
            is_cursor_value: true,
            cursor: retain_for_assembly(pointer),
            predefined: 0,
        }),
        _ => {
            if let Some(predefined) = keyword_of(data).and_then(crate::css::css_enums::keyword_to_cursor_predefined) {
                cursors.push(FfiCursorItemAssembly {
                    is_cursor_value: false,
                    cursor: std::ptr::null(),
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
        cursors.push(FfiCursorItemAssembly {
            is_cursor_value: false,
            cursor: std::ptr::null(),
            predefined: crate::css::css_enums::cursor_predefined::AUTO,
        });
    }

    const SCROLLBAR_COLOR_AUTO: u8 = 0;
    const SCROLLBAR_COLOR_LOWERED: u8 = 1;
    const SCROLLBAR_COLOR_CPP: u8 = 2;
    let mut scrollbar_color_kind = SCROLLBAR_COLOR_AUTO;
    let mut scrollbar_thumb_color = 0u32;
    let mut scrollbar_track_color = 0u32;
    if let Some(StyleValueData::ScrollbarColor {
        thumb_color,
        track_color,
    }) = values.value(property_id::SCROLLBAR_COLOR)
    {
        match (to_color(thumb_color.data(), input), to_color(track_color.data(), input)) {
            (Some(thumb), Some(track)) => {
                scrollbar_color_kind = SCROLLBAR_COLOR_LOWERED;
                scrollbar_thumb_color = packed_color(thumb);
                scrollbar_track_color = packed_color(track);
            }
            _ => scrollbar_color_kind = SCROLLBAR_COLOR_CPP,
        }
    }

    let Some(StyleValueData::ColorScheme { schemes, only, .. }) = values.value(property_id::COLOR_SCHEME) else {
        unreachable!("a computed color-scheme is a color-scheme value");
    };
    let scheme_raws: Vec<usize> = schemes.as_slice().iter().map(|scheme| scheme.raw()).collect();

    let assembly = FfiInheritedUiGroupAssembly {
        cpp_context: cpp_assembler_context,
        caret_is_auto,
        caret_resolved,
        accent_is_auto,
        accent_resolved,
        cursors: cursors.as_ptr(),
        cursor_count: cursors.len(),
        scrollbar_color_kind,
        scrollbar_thumb_color,
        scrollbar_track_color,
        color_schemes: scheme_raws.as_ptr(),
        color_scheme_count: scheme_raws.len(),
        color_scheme_only: *only,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::INHERITED_UI,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// The inherited text group's complex members, pre-lowered for the
/// registered C++ assembler.
#[repr(C)]
pub struct FfiInheritedTextGroupAssembly {
    pub cpp_context: *const c_void,
    /// Whether the core resolved (and poked) -webkit-text-fill-color; the
    /// assembler's C++ arm resolves it otherwise.
    pub webkit_text_fill_color_resolved: bool,
    /// Whether the core poked the pixel spacings; the assembler's C++ arm
    /// resolves the normal keyword and font-relative forms otherwise.
    pub word_spacing_resolved: bool,
    pub letter_spacing_resolved: bool,
    pub text_shadows: *const FfiLoweredShadow,
    pub text_shadow_count: usize,
    pub underline_position_horizontal: u8,
    pub underline_position_vertical: u8,
    pub underline_offset_is_auto: bool,
    /// The retained text-underline-offset value when it is not auto.
    pub underline_offset: *const c_void,
    /// The retained text-indent length-percentage and its flags.
    pub text_indent: *const c_void,
    pub text_indent_each_line: bool,
    pub text_indent_hanging: bool,
    pub tab_size_is_number: bool,
    pub tab_size_number: f64,
    /// Raw CSSPixels.
    pub tab_size_px: i32,
}

/// Builds the inherited text group: the generic descriptor path first, then
/// the shadow, indent, underline and tab-size lowering through the
/// registered assembler.
unsafe fn build_inherited_text_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::INHERITED_TEXT,
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

    let webkit_data = values
        .value(property_id::_WEBKIT_TEXT_FILL_COLOR)
        .expect("the table holds -webkit-text-fill-color");

    let assembly = FfiInheritedTextGroupAssembly {
        cpp_context: cpp_assembler_context,
        webkit_text_fill_color_resolved: resolved_color(input, property_id::_WEBKIT_TEXT_FILL_COLOR, webkit_data)
            .is_some(),
        word_spacing_resolved: is_px_length(values, property_id::WORD_SPACING),
        letter_spacing_resolved: is_px_length(values, property_id::LETTER_SPACING),
        text_shadows: text_shadows.as_ptr(),
        text_shadow_count: text_shadows.len(),
        underline_position_horizontal: *horizontal,
        underline_position_vertical: *vertical,
        underline_offset_is_auto,
        underline_offset: if underline_offset_is_auto {
            std::ptr::null()
        } else {
            retain_for_assembly(values.pointer(property_id::TEXT_UNDERLINE_OFFSET))
        },
        text_indent: retain_for_assembly(length_percentage.pointer().cast()),
        text_indent_each_line: *each_line,
        text_indent_hanging: *hanging,
        tab_size_is_number,
        tab_size_number,
        tab_size_px,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::INHERITED_TEXT,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// One lowered overflow-clip-margin side.
#[repr(C)]
pub struct FfiOverflowClipMarginSideAssembly {
    pub has_visual_box: bool,
    pub visual_box: u8,
    /// The retained offset value; null when the side keeps its default.
    pub offset: *const c_void,
}

/// One lowered will-change entry: a contents/scroll-position keyword, or a
/// custom ident naming a property (a borrowed fly-string raw).
#[repr(C)]
pub struct FfiWillChangeEntryAssembly {
    pub is_keyword: bool,
    pub keyword: u16,
    pub ident_raw: usize,
}

/// The misc reset group's complex members, pre-lowered for the registered
/// C++ assembler.
#[repr(C)]
pub struct FfiMiscResetGroupAssembly {
    pub cpp_context: *const c_void,
    /// Whether the core resolved (and poked) a non-auto outline-color; the
    /// assembler's C++ arm resolves it otherwise.
    pub outline_color_is_auto: bool,
    pub outline_color_resolved: bool,
    /// The retained outline-offset value: both the resolved pixel offset and
    /// the group's style value member come from it.
    pub outline_offset: *const c_void,
    /// Top, right, bottom, left retained values.
    pub scroll_margin: [*const c_void; 4],
    pub scroll_padding: [*const c_void; 4],
    /// Left, top, right, bottom.
    pub overflow_clip_margin: [FfiOverflowClipMarginSideAssembly; 4],
    /// C++ Appearance codes: the compat-normalized appearance and the raw
    /// computed appearance.
    pub appearance: u8,
    pub computed_appearance: u8,
    /// Retained offset values of the object-position edges.
    pub object_position_x: *const c_void,
    pub object_position_y: *const c_void,
    pub has_view_transition_name: bool,
    /// A borrowed fly-string raw, alive across the build.
    pub view_transition_name_raw: usize,
    pub touch_action_allow_left: bool,
    pub touch_action_allow_right: bool,
    pub touch_action_allow_up: bool,
    pub touch_action_allow_down: bool,
    pub touch_action_allow_pinch_zoom: bool,
    pub touch_action_allow_other: bool,
    /// The retained column-height value.
    pub column_height: *const c_void,
    /// The retained shape-margin value.
    pub shape_margin: *const c_void,
    /// Whether shape-outside is non-initial; the assembler's C++ arm walks
    /// the stamped wrapper, whose images read style sheet context.
    pub shape_outside_noninitial: bool,
    /// The C++ ScrollbarGutter code.
    pub scrollbar_gutter: u8,
    pub will_change_is_auto: bool,
    pub will_change_entries: *const FfiWillChangeEntryAssembly,
    pub will_change_entry_count: usize,
}

/// Builds the misc reset group: the generic descriptor path first, then the
/// full lowering through the registered assembler.
unsafe fn build_misc_reset_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
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

    let retained_slot = |property: u16| retain_for_assembly(values.pointer(property));
    let retained_sides = |properties: [u16; 4]| properties.map(retained_slot);

    let overflow_clip_margin_side = |property: u16| -> FfiOverflowClipMarginSideAssembly {
        match values.value(property) {
            Some(StyleValueData::OverflowClipMargin {
                has_visual_box,
                visual_box,
                offset,
            }) => FfiOverflowClipMarginSideAssembly {
                has_visual_box: *has_visual_box,
                visual_box: *visual_box,
                offset: retain_for_assembly(offset.pointer().cast()),
            },
            _ => FfiOverflowClipMarginSideAssembly {
                has_visual_box: false,
                visual_box: 0,
                offset: std::ptr::null(),
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
    let position_offset = |edge: &crate::css::style_value::RetainedStyleValueData| -> *const c_void {
        let StyleValueData::Edge { offset, .. } = edge.data() else {
            unreachable!("a computed position component is an edge value");
        };
        retain_for_assembly(offset.pointer().cast())
    };

    let view_transition_name = match values.value(property_id::VIEW_TRANSITION_NAME) {
        Some(StyleValueData::CustomIdent { custom_ident }) => Some(custom_ident.raw()),
        _ => None,
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

    let shape_outside_noninitial = values.pointer(property_id::SHAPE_OUTSIDE)
        != crate::css::style_compute::initial_value_data(property_id::SHAPE_OUTSIDE).cast();

    let Some(StyleValueData::ScrollbarGutter {
        value: scrollbar_gutter,
    }) = values.value(property_id::SCROLLBAR_GUTTER)
    else {
        unreachable!("a computed scrollbar-gutter is a scrollbar-gutter value");
    };

    // will-change, with the extractor's rules: unknown property names are
    // skipped, auto is the empty entry list.
    let mut will_change_is_auto = false;
    let mut will_change_entries: Vec<FfiWillChangeEntryAssembly> = Vec::new();
    match values.value(property_id::WILL_CHANGE) {
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::AUTO => will_change_is_auto = true,
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for item in list.as_slice() {
                match item.data() {
                    StyleValueData::Keyword { keyword: code } => will_change_entries.push(FfiWillChangeEntryAssembly {
                        is_keyword: true,
                        keyword: *code,
                        ident_raw: 0,
                    }),
                    StyleValueData::CustomIdent { custom_ident } => {
                        will_change_entries.push(FfiWillChangeEntryAssembly {
                            is_keyword: false,
                            keyword: 0,
                            ident_raw: custom_ident.raw(),
                        });
                    }
                    _ => unreachable!("a computed will-change item is a keyword or a custom ident"),
                }
            }
        }
        _ => unreachable!("a computed will-change is auto or a value list"),
    }

    let outline_color_data = values
        .value(property_id::OUTLINE_COLOR)
        .expect("the table holds outline-color");
    let outline_color_is_auto =
        matches!(outline_color_data, StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO);

    let assembly = FfiMiscResetGroupAssembly {
        cpp_context: cpp_assembler_context,
        outline_color_is_auto,
        outline_color_resolved: to_color(outline_color_data, input).is_some(),
        outline_offset: retained_slot(property_id::OUTLINE_OFFSET),
        scroll_margin: retained_sides([
            property_id::SCROLL_MARGIN_TOP,
            property_id::SCROLL_MARGIN_RIGHT,
            property_id::SCROLL_MARGIN_BOTTOM,
            property_id::SCROLL_MARGIN_LEFT,
        ]),
        scroll_padding: retained_sides([
            property_id::SCROLL_PADDING_TOP,
            property_id::SCROLL_PADDING_RIGHT,
            property_id::SCROLL_PADDING_BOTTOM,
            property_id::SCROLL_PADDING_LEFT,
        ]),
        overflow_clip_margin: [
            overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_LEFT),
            overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_TOP),
            overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_RIGHT),
            overflow_clip_margin_side(property_id::OVERFLOW_CLIP_MARGIN_BOTTOM),
        ],
        appearance: normalized_appearance,
        computed_appearance,
        object_position_x: position_offset(edge_x),
        object_position_y: position_offset(edge_y),
        has_view_transition_name: view_transition_name.is_some(),
        view_transition_name_raw: view_transition_name.unwrap_or(0),
        touch_action_allow_left: allow[0],
        touch_action_allow_right: allow[1],
        touch_action_allow_up: allow[2],
        touch_action_allow_down: allow[3],
        touch_action_allow_pinch_zoom: allow[4],
        touch_action_allow_other: allow[5],
        column_height: retained_slot(property_id::COLUMN_HEIGHT),
        shape_margin: retained_slot(property_id::SHAPE_MARGIN),
        shape_outside_noninitial,
        scrollbar_gutter: *scrollbar_gutter,
        will_change_is_auto,
        will_change_entries: will_change_entries.as_ptr(),
        will_change_entry_count: will_change_entries.len(),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::MISC_RESET,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// The C++ StyleValueList::Separator::Comma discriminant, static-asserted in
/// ComputedValues.cpp.
const SEPARATOR_COMMA: u8 = 1;

/// The text reset group's complex members, pre-lowered for the registered
/// C++ assembler. A color the core could not resolve falls to the
/// assembler's C++ arm.
#[repr(C)]
pub struct FfiTextResetGroupAssembly {
    pub cpp_context: *const c_void,
    pub text_decoration_color_resolved: bool,
    /// C++ TextDecorationLine codes; none is the empty list.
    pub text_decoration_lines: *const u8,
    pub text_decoration_line_count: usize,
    /// 0 = auto, 1 = from-font, 2 = a retained length-percentage value.
    pub text_decoration_thickness_kind: u8,
    pub text_decoration_thickness: *const c_void,
    pub white_space_trim_discard_before: bool,
    pub white_space_trim_discard_after: bool,
    pub white_space_trim_discard_inner: bool,
}

/// Builds the text reset group: the generic descriptor path first, then the
/// decoration-line, thickness and white-space-trim lowering through the
/// registered assembler.
unsafe fn build_text_reset_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    cpp_assembler_context: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic = unsafe {
        build_generic_group(
            group_index::TEXT_RESET,
            values,
            input,
            used_color_scheme,
            parent_payload,
        )
    };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::TEXT_RESET, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

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
        Some(_) => (
            2u8,
            retain_for_assembly(values.pointer(property_id::TEXT_DECORATION_THICKNESS)),
        ),
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

    let color_data = values
        .value(property_id::TEXT_DECORATION_COLOR)
        .expect("the table holds text-decoration-color");

    let assembly = FfiTextResetGroupAssembly {
        cpp_context: cpp_assembler_context,
        text_decoration_color_resolved: to_color(color_data, input).is_some(),
        text_decoration_lines: text_decoration_lines.as_ptr(),
        text_decoration_line_count: text_decoration_lines.len(),
        text_decoration_thickness_kind: thickness_kind,
        text_decoration_thickness: thickness_value,
        white_space_trim_discard_before: discard_before,
        white_space_trim_discard_after: discard_after,
        white_space_trim_discard_inner: discard_inner,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::TEXT_RESET,
            &entries,
            (&raw const assembly).cast(),
            parent_payload,
        )
    }
}

/// One position-try-fallbacks item, mirroring PositionTryFallbackData; the
/// name raw is borrowed and the area keywords are C++ PositionArea codes.
#[repr(C)]
pub struct FfiPositionTryFallbackAssembly {
    pub has_name: bool,
    pub name_raw: usize,
    pub tactics: [u8; 3],
    pub tactic_count: usize,
    pub has_position_area: bool,
    pub position_area_keywords: *const u8,
    pub position_area_keyword_count: usize,
}

/// The anchor group's members, pre-lowered for the registered C++ assembler.
/// Every fly-string raw is borrowed, alive across the build.
#[repr(C)]
pub struct FfiAnchorGroupAssembly {
    pub anchor_names: *const usize,
    pub anchor_name_count: usize,
    pub anchor_scope_all: bool,
    pub anchor_scope_names: *const usize,
    pub anchor_scope_name_count: usize,
    /// The C++ PositionAnchor::Type code.
    pub position_anchor_type: u8,
    pub position_anchor_name_raw: usize,
    /// C++ PositionArea codes.
    pub position_area_keywords: *const u8,
    pub position_area_keyword_count: usize,
    pub position_try_fallbacks: *const FfiPositionTryFallbackAssembly,
    pub position_try_fallback_count: usize,
    pub has_position_try_order: bool,
    pub position_try_order: u8,
    pub position_visibility_always: bool,
    pub position_visibility_anchors_valid: bool,
    pub position_visibility_anchors_visible: bool,
    pub position_visibility_no_overflow: bool,
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

/// Builds the anchor group: the generic descriptor path first, then the
/// name, area, try-fallback and visibility lowering through the registered
/// assembler.
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

    let anchor_names = lower_custom_ident_raws(values, property_id::ANCHOR_NAME);
    let anchor_scope_all = matches!(
        values.value(property_id::ANCHOR_SCOPE),
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::ALL
    );
    let anchor_scope_names = lower_custom_ident_raws(values, property_id::ANCHOR_SCOPE);

    let (position_anchor_type, position_anchor_name_raw) = match values.value(property_id::POSITION_ANCHOR) {
        Some(StyleValueData::CustomIdent { custom_ident }) => (3u8, custom_ident.raw()),
        Some(StyleValueData::Keyword { keyword: code }) => match *code {
            keyword::NORMAL => (0u8, 0),
            keyword::NONE => (1u8, 0),
            keyword::AUTO => (2u8, 0),
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
    let mut fallback_area_holders: Vec<Vec<u8>> = Vec::new();
    let mut position_try_fallbacks: Vec<FfiPositionTryFallbackAssembly> = Vec::new();
    let mut append_fallback = |data: &StyleValueData| {
        let mut fallback = FfiPositionTryFallbackAssembly {
            has_name: false,
            name_raw: 0,
            tactics: [0; 3],
            tactic_count: 0,
            has_position_area: false,
            position_area_keywords: std::ptr::null(),
            position_area_keyword_count: 0,
        };
        let apply_item = |fallback: &mut FfiPositionTryFallbackAssembly, data: &StyleValueData| {
            if let StyleValueData::CustomIdent { custom_ident } = data {
                fallback.has_name = true;
                fallback.name_raw = custom_ident.raw();
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
            fallback.position_area_keyword_count = area.len();
            fallback_area_holders.push(area);
            fallback.position_area_keywords = fallback_area_holders.last().expect("just pushed").as_ptr();
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

    let assembly = FfiAnchorGroupAssembly {
        anchor_names: anchor_names.as_ptr(),
        anchor_name_count: anchor_names.len(),
        anchor_scope_all,
        anchor_scope_names: anchor_scope_names.as_ptr(),
        anchor_scope_name_count: anchor_scope_names.len(),
        position_anchor_type,
        position_anchor_name_raw,
        position_area_keywords: position_area_keywords.as_ptr(),
        position_area_keyword_count: position_area_keywords.len(),
        position_try_fallbacks: position_try_fallbacks.as_ptr(),
        position_try_fallback_count: position_try_fallbacks.len(),
        has_position_try_order: position_try_order.is_some(),
        position_try_order: position_try_order.unwrap_or(0),
        position_visibility_always: always,
        position_visibility_anchors_valid: anchors_valid,
        position_visibility_anchors_visible: anchors_visible,
        position_visibility_no_overflow: no_overflow,
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::ANCHOR,
            &entries,
            (&raw const assembly).cast(),
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

/// Whether a live fly string's contents equal an ASCII string.
fn fly_string_equals_ascii(string: &RetainedUtf16FlyString, expected: &str) -> bool {
    crate::css::serialize::with_fly_string_units(string, |units| match units {
        crate::css::serialize::StringUnits::Ascii(bytes) => bytes == expected.as_bytes(),
        crate::css::serialize::StringUnits::Utf16(units) => {
            units.len() == expected.len() && units.iter().copied().eq(expected.bytes().map(u16::from))
        }
    })
}

/// The comma-separated computed items of a coordinated animation property:
/// the list's element pointers when the value is a comma-separated list, or
/// the single value itself, matching the C++ fallback's comma fan-out.
fn comma_item_pointers(values: &EffectiveValues, property: u16) -> Vec<*const c_void> {
    match values.value(property) {
        Some(StyleValueData::ValueList {
            values: list,
            separator,
            ..
        }) if *separator == SEPARATOR_COMMA => list.as_slice().iter().map(|value| value.pointer().cast()).collect(),
        _ => vec![values.pointer(property)],
    }
}

/// One comma-list time item: the plain value and unit when the slot holds a
/// time, or the retained calculated value for the assembler's
/// Time::from_style_value arm.
#[repr(C)]
pub struct FfiTimeItemAssembly {
    pub is_plain: bool,
    pub value: f64,
    /// The C++ TimeUnit code.
    pub unit: u8,
    /// The retained calculated value when not plain.
    pub calculated: *const c_void,
}

fn lower_time_item(pointer: *const c_void) -> FfiTimeItemAssembly {
    // SAFETY: The pointer names live table or override data.
    match unsafe { pointer.cast::<StyleValueData>().as_ref() } {
        Some(StyleValueData::Time { value, unit }) => FfiTimeItemAssembly {
            is_plain: true,
            value: *value,
            unit: *unit,
            calculated: std::ptr::null(),
        },
        _ => FfiTimeItemAssembly {
            is_plain: false,
            value: 0.0,
            unit: 0,
            calculated: retain_for_assembly(pointer),
        },
    }
}

/// One animation-name item; the name raw is borrowed, alive across the build.
#[repr(C)]
pub struct FfiAnimationNameAssembly {
    pub has_name: bool,
    pub name_raw: usize,
    pub is_string: bool,
}

/// One animation-duration item: auto or a time.
#[repr(C)]
pub struct FfiDurationItemAssembly {
    pub is_auto: bool,
    pub time: FfiTimeItemAssembly,
}

/// One animation-iteration-count item: the plain count, or the retained
/// calculated value for the assembler's number-resolution arm.
#[repr(C)]
pub struct FfiNumberItemAssembly {
    pub is_plain: bool,
    pub number: f64,
    /// The retained calculated value when not plain.
    pub value: *const c_void,
}

/// One animation-timeline item, mirroring AnimationTimelineData: the kind is
/// the C++ Type code, the name raw is borrowed, and the inset edges are
/// retained values for the assembler.
#[repr(C)]
pub struct FfiAnimationTimelineAssembly {
    pub kind: u8,
    pub name_raw: usize,
    pub has_scroller: bool,
    pub scroller: u8,
    pub has_axis: bool,
    pub axis: u8,
    pub has_inset: bool,
    pub inset_start: *const c_void,
    pub inset_end: *const c_void,
}

/// One optional timeline or transition-property name; the raw is borrowed.
#[repr(C)]
pub struct FfiTimelineNameAssembly {
    pub has_name: bool,
    pub name_raw: usize,
}

/// One view-timeline-inset item's retained start and end values.
#[repr(C)]
pub struct FfiViewTimelineInsetAssembly {
    pub start: *const c_void,
    pub end: *const c_void,
}

/// The animation group's members, pre-lowered for the registered C++
/// assembler. The timing-function entries are retained easing values the
/// assembler wraps and decodes.
#[repr(C)]
pub struct FfiAnimationGroupAssembly {
    pub names: *const FfiAnimationNameAssembly,
    pub name_count: usize,
    /// C++ AnimationComposition codes.
    pub compositions: *const u8,
    pub composition_count: usize,
    pub delays: *const FfiTimeItemAssembly,
    pub delay_count: usize,
    /// C++ AnimationDirection codes.
    pub directions: *const u8,
    pub direction_count: usize,
    pub durations: *const FfiDurationItemAssembly,
    pub duration_count: usize,
    /// C++ AnimationFillMode codes.
    pub fill_modes: *const u8,
    pub fill_mode_count: usize,
    pub iteration_counts: *const FfiNumberItemAssembly,
    pub iteration_count_count: usize,
    /// C++ AnimationPlayState codes.
    pub play_states: *const u8,
    pub play_state_count: usize,
    pub timelines: *const FfiAnimationTimelineAssembly,
    pub timeline_count: usize,
    /// Retained easing values.
    pub timing_functions: *const *const c_void,
    pub timing_function_count: usize,
    pub scroll_timeline_names: *const FfiTimelineNameAssembly,
    pub scroll_timeline_name_count: usize,
    /// C++ Axis codes.
    pub scroll_timeline_axes: *const u8,
    pub scroll_timeline_axis_count: usize,
    pub timeline_scope_all: bool,
    /// Borrowed fly-string raws.
    pub timeline_scope_names: *const usize,
    pub timeline_scope_name_count: usize,
    pub view_timeline_names: *const FfiTimelineNameAssembly,
    pub view_timeline_name_count: usize,
    /// C++ Axis codes.
    pub view_timeline_axes: *const u8,
    pub view_timeline_axis_count: usize,
    pub view_timeline_insets: *const FfiViewTimelineInsetAssembly,
    pub view_timeline_inset_count: usize,
    pub transition_properties: *const FfiTimelineNameAssembly,
    pub transition_property_count: usize,
    pub transition_durations: *const FfiTimeItemAssembly,
    pub transition_duration_count: usize,
    /// Retained easing values.
    pub transition_timing_functions: *const *const c_void,
    pub transition_timing_function_count: usize,
    pub transition_delays: *const FfiTimeItemAssembly,
    pub transition_delay_count: usize,
    /// C++ TransitionBehavior codes.
    pub transition_behaviors: *const u8,
    pub transition_behavior_count: usize,
}

fn lower_animation_timeline(pointer: *const c_void) -> FfiAnimationTimelineAssembly {
    let mut assembly = FfiAnimationTimelineAssembly {
        kind: 0,
        name_raw: 0,
        has_scroller: false,
        scroller: 0,
        has_axis: false,
        axis: 0,
        has_inset: false,
        inset_start: std::ptr::null(),
        inset_end: std::ptr::null(),
    };
    // SAFETY: The pointer names live table or override data.
    let data = unsafe { pointer.cast::<StyleValueData>().as_ref() }.expect("the table holds animation-timeline");
    match data {
        StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO => {}
        StyleValueData::Keyword { keyword: code } if *code == keyword::NONE => assembly.kind = 1,
        StyleValueData::CustomIdent { custom_ident } => {
            assembly.kind = 2;
            assembly.name_raw = custom_ident.raw();
        }
        StyleValueData::Function { name, value } => {
            let StyleValueData::Tuple { values: arguments } = value.data() else {
                unreachable!("a computed timeline function holds a tuple");
            };
            let arguments = arguments.as_slice();
            let argument = |index: usize| arguments.get(index).filter(|argument| !argument.pointer().is_null());
            let keyword_argument = |index: usize, map: fn(u16) -> Option<u8>| {
                argument(index).map(|argument| {
                    keyword_of(argument.data())
                        .and_then(map)
                        .expect("a computed timeline function argument maps to its enum")
                })
            };
            if fly_string_equals_ascii(name, "scroll") {
                assembly.kind = 3;
                if let Some(scroller) = keyword_argument(0, crate::css::css_enums::keyword_to_scroller) {
                    assembly.has_scroller = true;
                    assembly.scroller = scroller;
                }
                if let Some(axis) = keyword_argument(1, crate::css::css_enums::keyword_to_axis) {
                    assembly.has_axis = true;
                    assembly.axis = axis;
                }
            } else {
                assert!(
                    fly_string_equals_ascii(name, "view"),
                    "a computed timeline function is scroll() or view()"
                );
                assembly.kind = 4;
                if let Some(axis) = keyword_argument(0, crate::css::css_enums::keyword_to_axis) {
                    assembly.has_axis = true;
                    assembly.axis = axis;
                }
                if let Some(inset) = argument(1) {
                    let StyleValueData::ValueList { values: edges, .. } = inset.data() else {
                        unreachable!("a computed view() inset is a value list");
                    };
                    let edges = edges.as_slice();
                    assert!(edges.len() == 2, "a computed view() inset holds two edges");
                    assembly.has_inset = true;
                    assembly.inset_start = retain_for_assembly(edges[0].pointer().cast());
                    assembly.inset_end = retain_for_assembly(edges[1].pointer().cast());
                }
            }
        }
        _ => unreachable!("a computed animation-timeline item is auto, none, a name or a function"),
    }
    assembly
}

/// Lowers a keyword-coordinated animation list into its C++ enum codes.
fn lower_keyword_codes(values: &EffectiveValues, property: u16, map: fn(u16) -> Option<u8>) -> Vec<u8> {
    comma_item_pointers(values, property)
        .into_iter()
        .map(|pointer| {
            // SAFETY: The pointer names live table or override data.
            let data = unsafe { pointer.cast::<StyleValueData>().as_ref() }.expect("the table holds the property");
            keyword_of(data)
                .and_then(map)
                .expect("a computed coordinated keyword maps to its enum")
        })
        .collect()
}

/// The optional custom-ident names of a timeline-name-shaped property,
/// fanning out any value list like the C++ fallback's helpers.
fn lower_optional_name_list(values: &EffectiveValues, property: u16) -> Vec<FfiTimelineNameAssembly> {
    let lower = |data: &StyleValueData| match data {
        StyleValueData::CustomIdent { custom_ident } => FfiTimelineNameAssembly {
            has_name: true,
            name_raw: custom_ident.raw(),
        },
        _ => FfiTimelineNameAssembly {
            has_name: false,
            name_raw: 0,
        },
    };
    match values.value(property) {
        Some(StyleValueData::ValueList { values: list, .. }) => {
            list.as_slice().iter().map(|item| lower(item.data())).collect()
        }
        Some(data) => vec![lower(data)],
        None => unreachable!("the table holds the property"),
    }
}

/// The custom-ident raws of a scope-shaped property, skipping non-ident items
/// like the C++ fallback's custom_ident_list.
fn lower_custom_ident_raws(values: &EffectiveValues, property: u16) -> Vec<usize> {
    let mut raws = Vec::new();
    let mut append = |data: &StyleValueData| {
        if let StyleValueData::CustomIdent { custom_ident } = data {
            raws.push(custom_ident.raw());
        }
    };
    match values.value(property) {
        Some(StyleValueData::ValueList { values: list, .. }) => {
            for item in list.as_slice() {
                append(item.data());
            }
        }
        Some(data) => append(data),
        None => unreachable!("the table holds the property"),
    }
    raws
}

/// Builds the animation group: the generic descriptor path first, then the
/// full coordinated-list lowering through the registered assembler.
unsafe fn build_animation_group(
    values: &EffectiveValues,
    input: &ColorResolutionInput,
    used_color_scheme: u8,
    parent_payload: *const c_void,
) -> *const c_void {
    let generic =
        unsafe { build_generic_group(group_index::ANIMATION, values, input, used_color_scheme, parent_payload) };
    if !generic.is_null() {
        return generic;
    }
    let Some(entries) = (unsafe { gather_group_entries(group_index::ANIMATION, values, input, used_color_scheme) })
    else {
        return std::ptr::null();
    };

    // animation-name computes to a list; none is the empty entry.
    let names: Vec<FfiAnimationNameAssembly> = match values.value(property_id::ANIMATION_NAME) {
        Some(StyleValueData::ValueList { values: list, .. }) => list
            .as_slice()
            .iter()
            .map(|item| match item.data() {
                StyleValueData::Keyword { keyword: code } if *code == keyword::NONE => FfiAnimationNameAssembly {
                    has_name: false,
                    name_raw: 0,
                    is_string: false,
                },
                StyleValueData::String { string } => FfiAnimationNameAssembly {
                    has_name: true,
                    name_raw: string.raw(),
                    is_string: true,
                },
                StyleValueData::CustomIdent { custom_ident } => FfiAnimationNameAssembly {
                    has_name: true,
                    name_raw: custom_ident.raw(),
                    is_string: false,
                },
                _ => unreachable!("a computed animation-name item is none, a string or a custom ident"),
            })
            .collect(),
        _ => unreachable!("a computed animation-name is a value list"),
    };

    let time_items = |property: u16| -> Vec<FfiTimeItemAssembly> {
        comma_item_pointers(values, property)
            .into_iter()
            .map(lower_time_item)
            .collect()
    };
    let delays = time_items(property_id::ANIMATION_DELAY);
    let durations: Vec<FfiDurationItemAssembly> = comma_item_pointers(values, property_id::ANIMATION_DURATION)
        .into_iter()
        .map(|pointer| {
            // SAFETY: The pointer names live table or override data.
            let is_auto = matches!(
                unsafe { pointer.cast::<StyleValueData>().as_ref() },
                Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::AUTO
            );
            FfiDurationItemAssembly {
                is_auto,
                time: if is_auto {
                    FfiTimeItemAssembly {
                        is_plain: true,
                        value: 0.0,
                        unit: 0,
                        calculated: std::ptr::null(),
                    }
                } else {
                    lower_time_item(pointer)
                },
            }
        })
        .collect();
    let iteration_counts: Vec<FfiNumberItemAssembly> =
        comma_item_pointers(values, property_id::ANIMATION_ITERATION_COUNT)
            .into_iter()
            .map(|pointer| {
                // SAFETY: The pointer names live table or override data.
                match unsafe { pointer.cast::<StyleValueData>().as_ref() } {
                    Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::INFINITE => {
                        FfiNumberItemAssembly {
                            is_plain: true,
                            number: f64::INFINITY,
                            value: std::ptr::null(),
                        }
                    }
                    Some(StyleValueData::Number { value }) => FfiNumberItemAssembly {
                        is_plain: true,
                        number: *value,
                        value: std::ptr::null(),
                    },
                    _ => FfiNumberItemAssembly {
                        is_plain: false,
                        number: 0.0,
                        value: retain_for_assembly(pointer),
                    },
                }
            })
            .collect();
    let timelines: Vec<FfiAnimationTimelineAssembly> = comma_item_pointers(values, property_id::ANIMATION_TIMELINE)
        .into_iter()
        .map(lower_animation_timeline)
        .collect();
    let retained_items = |property: u16| -> Vec<*const c_void> {
        comma_item_pointers(values, property)
            .into_iter()
            .map(retain_for_assembly)
            .collect()
    };
    let timing_functions = retained_items(property_id::ANIMATION_TIMING_FUNCTION);

    let compositions = lower_keyword_codes(
        values,
        property_id::ANIMATION_COMPOSITION,
        crate::css::css_enums::keyword_to_animation_composition,
    );
    let directions = lower_keyword_codes(
        values,
        property_id::ANIMATION_DIRECTION,
        crate::css::css_enums::keyword_to_animation_direction,
    );
    let fill_modes = lower_keyword_codes(
        values,
        property_id::ANIMATION_FILL_MODE,
        crate::css::css_enums::keyword_to_animation_fill_mode,
    );
    let play_states = lower_keyword_codes(
        values,
        property_id::ANIMATION_PLAY_STATE,
        crate::css::css_enums::keyword_to_animation_play_state,
    );

    // The timeline-name and axis helpers fan out any value list.
    let axis_codes = |property: u16| -> Vec<u8> {
        let lower = |data: &StyleValueData| {
            keyword_of(data)
                .and_then(crate::css::css_enums::keyword_to_axis)
                .expect("a computed timeline axis maps to its enum")
        };
        match values.value(property) {
            Some(StyleValueData::ValueList { values: list, .. }) => {
                list.as_slice().iter().map(|item| lower(item.data())).collect()
            }
            Some(data) => vec![lower(data)],
            None => unreachable!("the table holds the property"),
        }
    };
    let scroll_timeline_names = lower_optional_name_list(values, property_id::SCROLL_TIMELINE_NAME);
    let scroll_timeline_axes = axis_codes(property_id::SCROLL_TIMELINE_AXIS);
    let view_timeline_names = lower_optional_name_list(values, property_id::VIEW_TIMELINE_NAME);
    let view_timeline_axes = axis_codes(property_id::VIEW_TIMELINE_AXIS);

    let timeline_scope_all = matches!(
        values.value(property_id::TIMELINE_SCOPE),
        Some(StyleValueData::Keyword { keyword: code }) if *code == keyword::ALL
    );
    let timeline_scope_names = lower_custom_ident_raws(values, property_id::TIMELINE_SCOPE);

    // view-timeline-inset computes to a value list: comma-separated inset
    // items, or itself the single two-edge inset.
    let lower_inset = |data: &StyleValueData| -> FfiViewTimelineInsetAssembly {
        let StyleValueData::ValueList { values: edges, .. } = data else {
            unreachable!("a computed view-timeline-inset item is a value list");
        };
        let edges = edges.as_slice();
        assert!(edges.len() == 2, "a computed view-timeline-inset item holds two edges");
        FfiViewTimelineInsetAssembly {
            start: retain_for_assembly(edges[0].pointer().cast()),
            end: retain_for_assembly(edges[1].pointer().cast()),
        }
    };
    let view_timeline_insets: Vec<FfiViewTimelineInsetAssembly> = match values.value(property_id::VIEW_TIMELINE_INSET) {
        Some(
            list_data @ StyleValueData::ValueList {
                values: list,
                separator,
                ..
            },
        ) => {
            if *separator == SEPARATOR_COMMA {
                list.as_slice().iter().map(|item| lower_inset(item.data())).collect()
            } else {
                vec![lower_inset(list_data)]
            }
        }
        _ => unreachable!("a computed view-timeline-inset is a value list"),
    };

    let transition_properties: Vec<FfiTimelineNameAssembly> =
        comma_item_pointers(values, property_id::TRANSITION_PROPERTY)
            .into_iter()
            .map(|pointer| {
                // SAFETY: The pointer names live table or override data.
                match unsafe { pointer.cast::<StyleValueData>().as_ref() } {
                    Some(StyleValueData::CustomIdent { custom_ident }) => FfiTimelineNameAssembly {
                        has_name: true,
                        name_raw: custom_ident.raw(),
                    },
                    _ => FfiTimelineNameAssembly {
                        has_name: false,
                        name_raw: 0,
                    },
                }
            })
            .collect();
    let transition_durations = time_items(property_id::TRANSITION_DURATION);
    let transition_timing_functions = retained_items(property_id::TRANSITION_TIMING_FUNCTION);
    let transition_delays = time_items(property_id::TRANSITION_DELAY);
    let transition_behaviors = lower_keyword_codes(
        values,
        property_id::TRANSITION_BEHAVIOR,
        crate::css::css_enums::keyword_to_transition_behavior,
    );

    let assembly = FfiAnimationGroupAssembly {
        names: names.as_ptr(),
        name_count: names.len(),
        compositions: compositions.as_ptr(),
        composition_count: compositions.len(),
        delays: delays.as_ptr(),
        delay_count: delays.len(),
        directions: directions.as_ptr(),
        direction_count: directions.len(),
        durations: durations.as_ptr(),
        duration_count: durations.len(),
        fill_modes: fill_modes.as_ptr(),
        fill_mode_count: fill_modes.len(),
        iteration_counts: iteration_counts.as_ptr(),
        iteration_count_count: iteration_counts.len(),
        play_states: play_states.as_ptr(),
        play_state_count: play_states.len(),
        timelines: timelines.as_ptr(),
        timeline_count: timelines.len(),
        timing_functions: timing_functions.as_ptr(),
        timing_function_count: timing_functions.len(),
        scroll_timeline_names: scroll_timeline_names.as_ptr(),
        scroll_timeline_name_count: scroll_timeline_names.len(),
        scroll_timeline_axes: scroll_timeline_axes.as_ptr(),
        scroll_timeline_axis_count: scroll_timeline_axes.len(),
        timeline_scope_all,
        timeline_scope_names: timeline_scope_names.as_ptr(),
        timeline_scope_name_count: timeline_scope_names.len(),
        view_timeline_names: view_timeline_names.as_ptr(),
        view_timeline_name_count: view_timeline_names.len(),
        view_timeline_axes: view_timeline_axes.as_ptr(),
        view_timeline_axis_count: view_timeline_axes.len(),
        view_timeline_insets: view_timeline_insets.as_ptr(),
        view_timeline_inset_count: view_timeline_insets.len(),
        transition_properties: transition_properties.as_ptr(),
        transition_property_count: transition_properties.len(),
        transition_durations: transition_durations.as_ptr(),
        transition_duration_count: transition_durations.len(),
        transition_timing_functions: transition_timing_functions.as_ptr(),
        transition_timing_function_count: transition_timing_functions.len(),
        transition_delays: transition_delays.as_ptr(),
        transition_delay_count: transition_delays.len(),
        transition_behaviors: transition_behaviors.as_ptr(),
        transition_behavior_count: transition_behaviors.len(),
    };
    // SAFETY: The entries and assembly hold live or retained value data, and
    // the caller warrants the parent payload.
    unsafe {
        crate::css::computed_values::build_group_payload_with_assembler(
            group_index::ANIMATION,
            &entries,
            (&raw const assembly).cast(),
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
/// The font group always stays with C++ (its payload is not a Rust group).
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
                    group_index::FONT => std::ptr::null(),
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
                    group_index::TEXT_RESET => build_text_reset_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::ALIGNMENT => build_alignment_group(&values, parent_payload),
                    group_index::SVG_RESET => build_svg_reset_group(&values, &input, parent_payload),
                    group_index::GRID => build_grid_group(&values, parent_payload),
                    group_index::TRANSFORM => {
                        build_transform_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::EFFECTS => {
                        build_effects_group(&values, &input, inputs.used_color_scheme, parent_payload)
                    }
                    group_index::INHERITED_UI => build_inherited_ui_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::INHERITED_TEXT => build_inherited_text_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::MISC_RESET => build_misc_reset_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::INHERITED_SVG => build_inherited_svg_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::INHERITED_LIST => build_inherited_list_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::CONTENT => build_content_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::BACKGROUND => build_background_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::MASK => build_mask_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
                    group_index::BORDER => build_border_group(
                        &values,
                        &input,
                        inputs.used_color_scheme,
                        inputs.cpp_assembler_context,
                        parent_payload,
                    ),
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
