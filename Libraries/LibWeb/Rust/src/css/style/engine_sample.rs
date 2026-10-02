/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the engine needs to sample an element's animations from its own records and the document's
//! published inputs, rather than from the host's working set.

use super::bridge::element_adjustment_fact;
use super::publication::drive_font_metric;
use super::tree::{StyleNodeID, TreeScopeID};
use super::{RetainedState, bridge};
use crate::css::animated_overlay::AnimatedOverlay;
use crate::css::color_resolution::{ColorResolutionInput, FfiColorResolutionInput, Rgba, to_color};
use crate::css::computed_longhand_table::{ComputedLonghandTable, FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS};
use crate::css::computed_value_types::{
    FontValues, STYLE_GROUP_INDEX_ANCHOR, STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX,
    STYLE_GROUP_INDEX_SURROUND,
};
use crate::css::computed_values::InheritedBoxValues;
use crate::css::css_pixels::CssPixels;
use crate::css::host_shared::SharedPayload;
use crate::css::property_metadata::property_id as prop;
use crate::css::style_compute::{
    FfiAnimationLengthContexts, FfiFontMetrics, FfiLengthResolutionContext, keyword, px_length_unit,
};
use crate::css::style_value::StyleValueData;
use crate::css::table_group_builder::{FfiFontGroupBuildInputs, FfiTableGroupBuildInputs, group_index};

/// One record's font, as a length resolves against it.
struct RecordFont {
    metrics: FfiFontMetrics,
    depends_on_viewport_metrics: bool,
    inline_axis_is_horizontal: bool,
}

impl RetainedState {
    fn record_font(&self, style_record: u64) -> Option<RecordFont> {
        let view = self.computed_group_sets.style_record_view(style_record)?;
        let font = unsafe { view.payloads[STYLE_GROUP_INDEX_FONT].cast::<FontValues>().deref() };
        let inherited_box = unsafe {
            view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]
                .cast::<InheritedBoxValues>()
                .deref()
        };
        Some(RecordFont {
            metrics: FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: drive_font_metric(font.font_x_height),
                // The host's metrics approximate the cap height with the ascent.
                cap_height: drive_font_metric(font.font_ascent),
                zero_advance: drive_font_metric(font.font_zero_advance),
                line_height: font.line_height_used.to_double(),
            },
            depends_on_viewport_metrics: view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
            inline_axis_is_horizontal: inherited_box.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
        })
    }

    /// The font a `rem` resolves against, and whether it reads the viewport: the font of the record
    /// the host holds for the document element, which the document inputs only describe as of the
    /// transaction they were published with.
    fn root_font_metrics(&self) -> (FfiFontMetrics, bool) {
        self.held_root_font_inputs
            .unwrap_or_else(|| {
                super::publication::RootFontInputs::from_document(&self.document_style_computation_inputs)
            })
            .font_metrics()
    }

    /// The three length-resolution contexts a sample of an element's animations computes keyframe
    /// values in over `style_record`, the record the host holds for it: the font context, which
    /// reads the record its inheritance parent holds; the line-height context, the element's own
    /// font with the line height it inherits; and the one everything else resolves against, the
    /// element's own font. A mirror of the host's `get_computation_context_for_property(FontFamily /
    /// LineHeight / Color)` over the working set it reconstructs from that record, without the
    /// container bases only the host resolves.
    ///
    /// `None` where the engine cannot read the record.
    pub(crate) fn animation_sample_length_contexts(
        &self,
        node: StyleNodeID,
        pseudo_kind: Option<u8>,
        style_record: u64,
    ) -> Option<FfiAnimationLengthContexts> {
        let inputs = &self.document_style_computation_inputs;
        let own = self.record_font(style_record)?;
        // A pseudo-element inherits from its originating element.
        let parent = match pseudo_kind {
            Some(_) => Some(node),
            None => self.tree.inheritance_parent(node),
        };
        let parent_font = parent
            .and_then(|parent| self.held_style_records.get(&parent).copied())
            .and_then(|record| self.record_font(record));
        let is_document_element =
            self.computed_group_sets.adjustment_facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0;
        let document_root = self.root_font_metrics();
        let own_font = (own.metrics, own.depends_on_viewport_metrics);
        let length_context = |font, root| length_resolution_context(inputs, font, root, own.inline_axis_is_horizontal);

        // The font context is the parent's own, or the document's initial font's where there is no
        // parent to read. The initial line height is zero.
        let initial = (
            FfiFontMetrics {
                font_size: inputs.initial_font_size,
                x_height: inputs.initial_font_x_height,
                cap_height: inputs.initial_font_cap_height,
                zero_advance: inputs.initial_font_zero_advance,
                line_height: 0.0,
            },
            false,
        );
        let font = match &parent_font {
            Some(parent_font) => length_context(
                (parent_font.metrics, parent_font.depends_on_viewport_metrics),
                document_root,
            ),
            None => length_context(initial, initial),
        };

        // The line-height context is the element's own font with the line height it inherits, and a
        // `rem` on the document element names its own font.
        let line_height_font = (
            FfiFontMetrics {
                line_height: parent_font
                    .as_ref()
                    .map_or(0.0, |parent_font| parent_font.metrics.line_height),
                ..own.metrics
            },
            own.depends_on_viewport_metrics,
        );
        let line_height = length_context(
            line_height_font,
            if is_document_element {
                line_height_font
            } else {
                document_root
            },
        );

        let remaining = length_context(
            own_font,
            if is_document_element && pseudo_kind.is_none() {
                own_font
            } else {
                document_root
            },
        );
        Some(FfiAnimationLengthContexts {
            font,
            line_height,
            remaining,
        })
    }

    /// What a transition on an element resolves its lengths against: the font of the record the
    /// element has installed, a sampled animated font included, the root's font and the viewport.
    /// A mirror of the host's computation context for `color` over that record. `None` where the
    /// engine cannot read the record.
    pub(crate) fn transition_length_resolution_context(
        &self,
        style_record: u64,
    ) -> Option<crate::css::animation::FfiAnimationLengthResolutionContext> {
        let record = self.record_font(style_record)?;
        Some(crate::css::style_compute::animation_length_resolution_context(
            &length_resolution_context(
                &self.document_style_computation_inputs,
                (record.metrics, record.depends_on_viewport_metrics),
                self.root_font_metrics(),
                record.inline_axis_is_horizontal,
            ),
        ))
    }

    /// Compose an element's sampled animation overlay into the payloads of its overlay record,
    /// over `style_record`, the record it was sampled on: the groups a value of the overlay lives
    /// in, and the groups that read an animated `color`, are rebuilt from `table` with the overlay
    /// applied, and every other group is the base record's. Writes every group's payload to
    /// `payloads` and answers which ones it rebuilt, each of which the caller owns a reference to,
    /// or `None` where the engine holds no such record.
    ///
    /// `font` supplies the platform font of the animated style, which only the host resolves,
    /// where the font group is rebuilt. The overlay is adjusted already, as the sample's animated
    /// box-type finalization leaves it.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn build_animation_overlay_payloads(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
        style_record: u64,
        table: &ComputedLonghandTable,
        overlay: Option<&AnimatedOverlay>,
        used_color_scheme: u8,
        display_before_box_type_transformation_raw: u32,
        font: impl FnOnce() -> FfiFontGroupBuildInputs,
        payloads: &mut [*const std::ffi::c_void; group_index::COUNT],
    ) -> Option<RebuiltOverlayGroups> {
        let view = self.computed_group_sets.style_record_view(style_record)?;
        payloads.copy_from_slice(SharedPayload::as_pointer_slice(view.base_payloads));
        // An overlay that animates nothing leaves the base as it is: publishing it releases the
        // element's overlay record.
        let Some(overlay) = overlay.filter(|overlay| !overlay.is_empty()) else {
            return Some(RebuiltOverlayGroups {
                groups: 0,
                every_group: true,
            });
        };
        // A value no group is known to hold, or an animated `color` whose readers the engine cannot
        // name, rebuilds every group.
        let named_groups = overlay.entries().iter().try_fold(0u32, |groups, entry| {
            let group = crate::css::property_metadata::property_style_group_index(entry.property)?;
            let readers = match entry.property {
                prop::COLOR => self.current_color_dependent_group_mask(node, pseudo_kind)?,
                _ => 0,
            };
            Some(groups | 1 << group | readers)
        });
        let rebuilds_every_group = named_groups.is_none();
        let mut groups = named_groups.unwrap_or((1 << group_index::COUNT) - 1);
        // The surround group duplicates `position-anchor` for layout, so rebuilding the anchor
        // group refreshes it too.
        if groups & (1 << STYLE_GROUP_INDEX_ANCHOR) != 0 {
            groups |= 1 << STYLE_GROUP_INDEX_SURROUND;
        }

        // Colors resolve against the element's font as it stood when the overlay was last
        // published, or against the animated font where the overlay rebuilds the font group.
        let record = self.record_font(style_record)?;
        let font_inputs = (groups & (1 << STYLE_GROUP_INDEX_FONT) != 0).then(font);
        let own_font = match &font_inputs {
            Some(font) => FfiFontMetrics {
                font_size: CssPixels::from_raw(font.font_size_raw).to_double(),
                x_height: drive_font_metric(font.font_x_height),
                cap_height: drive_font_metric(font.font_ascent),
                zero_advance: drive_font_metric(font.font_zero_advance),
                line_height: CssPixels::from_raw(font.line_height_used_raw).to_double(),
            },
            None => record.metrics,
        };
        let length = length_resolution_context(
            &self.document_style_computation_inputs,
            (own_font, record.depends_on_viewport_metrics),
            self.root_font_metrics(),
            record.inline_axis_is_horizontal,
        );
        // The element's own color resolves first, since every other group resolves `currentcolor`
        // against it.
        let color_value = table.effective_value(Some(overlay), prop::COLOR, true).value;
        let color_data = unsafe { color_value.cast::<StyleValueData>().as_ref() };
        let color = color_data
            .and_then(|color| {
                to_color(
                    color,
                    &ColorResolutionInput {
                        scheme: Some(used_color_scheme),
                        current_color: Some(Rgba::BLACK),
                        current_color_value: color_data,
                        length: Some(&length),
                        channels: None,
                    },
                )
            })
            .unwrap_or(Rgba::BLACK);
        let color_input = FfiColorResolutionInput {
            has_scheme: true,
            scheme: used_color_scheme,
            has_current_color: true,
            current_color_rgba: [color.r, color.g, color.b, color.a],
            current_color_value: color_value,
            length: (&raw const length).cast(),
            channels_present: [false; 13],
            channels: [0.0; 13],
            has_channels: false,
        };
        let build_inputs = FfiTableGroupBuildInputs {
            color_input: (&raw const color_input).cast(),
            used_color_scheme,
            animated_overlay: overlay,
            box_display_before_transformation_raw: display_before_box_type_transformation_raw,
            font: font_inputs.as_ref().map_or(std::ptr::null(), std::ptr::from_ref),
        };
        let parents = [std::ptr::null(); group_index::COUNT];
        let mut rebuilt = [std::ptr::null(); group_index::COUNT];
        unsafe {
            crate::css::table_group_builder::rust_build_group_payloads_from_table(
                table,
                groups,
                parents.as_ptr(),
                &raw const build_inputs,
                rebuilt.as_mut_ptr(),
                group_index::COUNT,
            );
        }
        let mut rebuilt_groups = 0;
        for (group, payload) in rebuilt.into_iter().enumerate() {
            if groups & (1 << group) != 0 && !payload.is_null() {
                payloads[group] = payload;
                rebuilt_groups |= 1 << group;
            }
        }
        Some(RebuiltOverlayGroups {
            groups: rebuilt_groups,
            every_group: rebuilds_every_group,
        })
    }
}

/// The groups `build_animation_overlay_payloads` rebuilt over the base record.
pub(crate) struct RebuiltOverlayGroups {
    /// The groups whose payloads the caller owns a reference to.
    pub(crate) groups: u32,
    /// Whether the overlay named a value the groups could not be told from, or nothing at all, so
    /// none of the base record's groups could be kept as they were.
    pub(crate) every_group: bool,
}

/// Give back the references to the payloads `build_animation_overlay_payloads` rebuilt.
pub(crate) fn release_rebuilt_overlay_payloads(payloads: &[*const std::ffi::c_void], groups: u32) {
    for (group, payload) in payloads.iter().enumerate() {
        if groups & (1 << group) != 0 {
            crate::css::computed_values::release_group_payload(group, *payload);
        }
    }
}

/// A length-resolution context over the document's viewport and the given fonts, without
/// container bases.
fn length_resolution_context(
    inputs: &bridge::FfiDocumentStyleComputationInputs,
    (font_metrics, font_metrics_depend_on_viewport_metrics): (FfiFontMetrics, bool),
    (root_font_metrics, root_font_metrics_depend_on_viewport_metrics): (FfiFontMetrics, bool),
    subject_inline_axis_is_horizontal: bool,
) -> FfiLengthResolutionContext {
    FfiLengthResolutionContext {
        viewport_width: inputs.viewport_width,
        viewport_height: inputs.viewport_height,
        font_metrics,
        root_font_metrics,
        font_metrics_depend_on_viewport_metrics,
        root_font_metrics_depend_on_viewport_metrics,
        has_container_width_basis: false,
        has_container_height_basis: false,
        container_width_basis: 0.0,
        container_height_basis: 0.0,
        container_width_basis_depends_on_viewport_metrics: false,
        container_height_basis_depends_on_viewport_metrics: false,
        subject_inline_axis_is_horizontal,
        resolved_viewport_relative_length: std::ptr::null_mut(),
    }
}

/// A longhand's computed value, over the overlay a sample composed where there is one.
pub(super) fn effective_data<'a>(
    table: &'a ComputedLonghandTable,
    overlay: Option<&'a AnimatedOverlay>,
    property: u16,
) -> Option<&'a StyleValueData> {
    unsafe {
        table
            .effective_value(overlay, property, true)
            .value
            .cast::<StyleValueData>()
            .as_ref()
    }
}

/// What resolving an element's font asks the document's resolver, as the C++ font computer would
/// resolve it for the element's computed values over `overlay`. The font phase computes
/// font-size, font-weight and font-width to the shapes read here. `feature_values_scope` names the
/// tree scope whose `@font-feature-values` the element's `font-variant-alternates` reads, asked
/// only when it has one.
pub(super) fn font_resolution_request(
    table: &ComputedLonghandTable,
    overlay: Option<&AnimatedOverlay>,
    inputs: &bridge::FfiDocumentStyleComputationInputs,
    feature_values_scope: impl FnOnce() -> TreeScopeID,
) -> bridge::FfiFontResolutionRequest {
    let value_of = |property| effective_data(table, overlay, property);
    let handle_of =
        |property| bridge::FfiHostHandle::from_pointer(table.effective_value(overlay, property, true).value.cast());
    // The resolver reads these beside the family, so the request names each one whose computed
    // value is not the initial one and nothing for the rest. A non-initial value can also come
    // from inheritance.
    let mut font_feature_values = [bridge::FfiHostHandle::default(); bridge::FONT_RESOLUTION_FEATURE_INPUT_COUNT];
    for (input, property, initial_keyword) in FONT_RESOLUTION_FEATURE_PROPERTIES {
        if !matches!(value_of(property),
            Some(StyleValueData::Keyword { keyword }) if *keyword == initial_keyword)
        {
            font_feature_values[input as usize] = handle_of(property);
        }
    }
    let font_size_raw = match value_of(prop::FONT_SIZE) {
        Some(StyleValueData::Length { value, unit }) if *unit == px_length_unit() => {
            CssPixels::nearest_value_for(*value).raw_value()
        }
        _ => unreachable!("the font phase left font-size uncomputed"),
    };
    let font_slope = match value_of(prop::FONT_STYLE) {
        Some(StyleValueData::FontStyle { font_style, .. }) => match *font_style {
            crate::css::css_enums::font_style_keyword::ITALIC => 1,
            crate::css::css_enums::font_style_keyword::OBLIQUE => 2,
            _ => 0,
        },
        _ => 0,
    };
    let (font_weight, font_width) = match (value_of(prop::FONT_WEIGHT), value_of(prop::FONT_WIDTH)) {
        (Some(StyleValueData::Number { value: weight }), Some(StyleValueData::Percentage { value: width })) => {
            (*weight, *width)
        }
        _ => unreachable!("the font phase left font-weight or font-width uncomputed"),
    };
    let font_optical_sizing = match value_of(prop::FONT_OPTICAL_SIZING) {
        Some(StyleValueData::Keyword { keyword }) => {
            crate::css::css_enums::keyword_to_font_optical_sizing(*keyword).unwrap_or(0)
        }
        _ => 0,
    };
    // Only `font-variant-alternates` reads `@font-feature-values`, so every other request resolves
    // once for all scopes.
    let names_alternates = !font_feature_values[bridge::FontResolutionFeatureInput::FontVariantAlternates as usize]
        .as_pointer()
        .is_null();
    let feature_values_scope = names_alternates
        .then(feature_values_scope)
        .unwrap_or(TreeScopeID::DOCUMENT);
    bridge::FfiFontResolutionRequest {
        font_family: handle_of(prop::FONT_FAMILY),
        font_feature_values,
        font_size_raw,
        font_slope,
        font_weight,
        font_width,
        font_optical_sizing,
        font_feature_values_scope: feature_values_scope.0,
        font_environment_generation: inputs.font_environment_generation,
    }
}

/// The property behind each value a font resolution request names, with the initial keyword for
/// which the request names nothing.
const FONT_RESOLUTION_FEATURE_PROPERTIES: [(bridge::FontResolutionFeatureInput, u16, u16);
    bridge::FONT_RESOLUTION_FEATURE_INPUT_COUNT] = {
    use crate::css::style_compute::keyword::{AUTO, NORMAL};
    use bridge::FontResolutionFeatureInput as Input;
    [
        (Input::FontFeatureSettings, prop::FONT_FEATURE_SETTINGS, NORMAL),
        (Input::FontVariationSettings, prop::FONT_VARIATION_SETTINGS, NORMAL),
        (Input::FontVariantCaps, prop::FONT_VARIANT_CAPS, NORMAL),
        (Input::FontVariantEastAsian, prop::FONT_VARIANT_EAST_ASIAN, NORMAL),
        (Input::FontVariantEmoji, prop::FONT_VARIANT_EMOJI, NORMAL),
        (Input::FontVariantLigatures, prop::FONT_VARIANT_LIGATURES, NORMAL),
        (Input::FontVariantNumeric, prop::FONT_VARIANT_NUMERIC, NORMAL),
        (Input::FontVariantPosition, prop::FONT_VARIANT_POSITION, NORMAL),
        (Input::FontVariantAlternates, prop::FONT_VARIANT_ALTERNATES, NORMAL),
        (Input::FontKerning, prop::FONT_KERNING, AUTO),
        (Input::TextRendering, prop::TEXT_RENDERING, AUTO),
    ]
};

// Every input appears once, at its own index, so no slot of a request goes unwritten.
const _: () = {
    let mut index = 0;
    while index < FONT_RESOLUTION_FEATURE_PROPERTIES.len() {
        assert!(FONT_RESOLUTION_FEATURE_PROPERTIES[index].0 as usize == index);
        index += 1;
    }
};

/// The line height `normal` uses for a resolved font.
pub(super) fn normal_line_height(resolved: &bridge::FfiResolvedFont) -> f64 {
    f64::from(resolved.ascent.round() as i32 + resolved.descent.round() as i32)
}

/// The used line height, as the C++ working set reads it from the computed value over `overlay`
/// against the font it resolved. The line-height phase computes line-height to one of these
/// shapes.
pub(super) fn used_line_height(
    table: &ComputedLonghandTable,
    overlay: Option<&AnimatedOverlay>,
    request: &bridge::FfiFontResolutionRequest,
    resolved: &bridge::FfiResolvedFont,
) -> f64 {
    match effective_data(table, overlay, prop::LINE_HEIGHT) {
        Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL => normal_line_height(resolved),
        Some(StyleValueData::Length { value, unit }) if *unit == px_length_unit() => {
            CssPixels::nearest_value_for(*value).to_double()
        }
        Some(StyleValueData::Number { value }) => CssPixels::nearest_value_for(value * request.font_size()).to_double(),
        _ => unreachable!("the line-height phase left line-height uncomputed"),
    }
}

/// What the font group of a record is built from, for an element whose font `request` resolved to
/// `resolved`, with the computed values over `overlay`.
pub(super) fn font_group_build_inputs(
    table: &ComputedLonghandTable,
    overlay: Option<&AnimatedOverlay>,
    request: &bridge::FfiFontResolutionRequest,
    line_height_used: f64,
    resolved: &bridge::FfiResolvedFont,
) -> FfiFontGroupBuildInputs {
    let keyword_code = |property, map: fn(u16) -> Option<u8>| match effective_data(table, overlay, property) {
        Some(StyleValueData::Keyword { keyword }) => map(*keyword).unwrap_or(0),
        _ => 0,
    };
    let math_depth = match effective_data(table, overlay, prop::MATH_DEPTH) {
        Some(StyleValueData::Integer { value }) => *value,
        _ => 0,
    };
    FfiFontGroupBuildInputs {
        font_size_raw: request.font_size_raw,
        line_height_used_raw: CssPixels::nearest_value_for(line_height_used).raw_value(),
        font_variant_emoji: keyword_code(
            prop::FONT_VARIANT_EMOJI,
            crate::css::css_enums::keyword_to_font_variant_emoji,
        ),
        font_ascent: resolved.ascent,
        font_descent: resolved.descent,
        font_x_height: resolved.x_height,
        font_zero_advance: resolved.zero_advance,
        first_available_font: resolved.first_available_font.as_pointer(),
        font_cascade_list: resolved.font_cascade_list.as_pointer(),
        font_weight: request.font_weight,
        font_width: request.font_width,
        math_shift: keyword_code(prop::MATH_SHIFT, crate::css::css_enums::keyword_to_math_shift),
        math_style: keyword_code(prop::MATH_STYLE, crate::css::css_enums::keyword_to_math_style),
        math_depth,
    }
}
