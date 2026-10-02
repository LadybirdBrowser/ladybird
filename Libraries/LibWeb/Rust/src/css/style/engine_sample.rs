/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the engine needs to sample an element's animations from its own records and the document's
//! published inputs, rather than from the host's working set.

use super::bridge::element_adjustment_fact;
use super::publication::drive_font_metric;
use super::tree::StyleNodeID;
use super::{RetainedState, bridge};
use crate::css::computed_longhand_table::FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS;
use crate::css::computed_value_types::{FontValues, STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
use crate::css::computed_values::InheritedBoxValues;
use crate::css::style_compute::{FfiAnimationLengthContexts, FfiFontMetrics, FfiLengthResolutionContext};

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
