/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl StyleEngine {
    /// Run the drive's remaining phase for the selected longhands over a copy of the node's
    /// current table, against the record's own font metrics, the document's computation inputs
    /// and the parent's record. The required driver inputs recompute on every drive and their
    /// post-compute adjustments read element facts this context does not carry, so the table
    /// stands only when they came out exactly as before.
    pub(super) fn engine_driven_table(
        &mut self,
        node: StyleNodeID,
        old_style_record: computed::FinalStyleRecordID,
        store: &CascadedPropertyStore,
        selected: &[u64],
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> Option<(
        ComputedLonghandTable,
        crate::css::style_compute::FfiLengthResolutionContext,
        u32,
        Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
    )> {
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiLengthResolutionContext, FfiStyleComputationEnvironment,
            LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation, empty_longhand_driver_results,
            is_required_driver_input, parent_snapshot_for_style_record, property_computation_order_for_phase,
        };

        let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecord);
            return None;
        };
        if !view.animated_overlay.is_null() {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        let Some(old_table) = (unsafe { view.longhand_table.as_ref() }) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecordTable);
            return None;
        };
        // A record under display:none may no longer be the style C++ holds, and a property change
        // on an element with active transitions starts one in the C++ computation.
        if view.dependency_flags & (1 << 2) != 0
            || !crate::css::style_compute::active_transition_properties(old_table).is_empty()
        {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        let snapshot = match self.tree.flat_tree_parent(node) {
            None => None,
            Some(parent) => match self.computed_group_sets.assigned_style_record(parent) {
                Some(record) => {
                    let parent_has_animation_overlay = self
                        .computed_group_sets
                        .style_record_view(record.raw())
                        .is_some_and(|view| !view.animated_overlay.is_null());
                    if parent_has_animation_overlay {
                        self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                        return None;
                    }
                    Some(parent_snapshot_for_style_record(self, record.raw(), None))
                }
                None => {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                }
            },
        };
        let font =
            unsafe { &*view.payloads[STYLE_GROUP_INDEX_FONT].cast::<crate::css::computed_value_types::FontValues>() };
        let inherited_box = unsafe {
            &*view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX].cast::<crate::css::computed_values::InheritedBoxValues>()
        };
        let mut resolved_viewport_relative_length = false;
        let length = FfiLengthResolutionContext {
            viewport_width: inputs.viewport_width,
            viewport_height: inputs.viewport_height,
            font_metrics: FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: drive_font_metric(font.font_x_height),
                // The C++ metrics approximate the cap height with the ascent.
                cap_height: drive_font_metric(font.font_ascent),
                zero_advance: drive_font_metric(font.font_zero_advance),
                line_height: font.line_height_used.to_double(),
            },
            root_font_metrics: FfiFontMetrics {
                font_size: inputs.root_font_size,
                x_height: inputs.root_font_x_height,
                cap_height: inputs.root_font_cap_height,
                zero_advance: inputs.root_font_zero_advance,
                line_height: inputs.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: view.dependency_flags & (1 << 1) != 0,
            root_font_metrics_depend_on_viewport_metrics: inputs.root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: inherited_box.writing_mode
                == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            resolved_viewport_relative_length: &raw mut resolved_viewport_relative_length,
        };
        // No element fact reaches the remaining phase through this environment: the moved
        // properties were checked not to need one, and the required driver inputs are compared
        // against the record below.
        let environment = FfiStyleComputationEnvironment {
            box_type_input: crate::css::style_compute::rust_box_type_transformation_input(
                0,
                crate::css::style_compute::FfiStyleAdjustmentTarget::Element,
                false,
                crate::css::display::FfiDisplay::block(),
            ),
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: 0,
                has_document_supported_schemes: false,
                document_supported_scheme_codes: std::ptr::null(),
                document_supported_scheme_count: 0,
            },
            is_th_element: false,
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };
        let mut table = view.longhand_table_for_partial_drive();
        let mut results = empty_longhand_driver_results();
        let mut effective_color_scheme = old_table.effective_color_scheme();
        unsafe {
            drive_property_computation(
                &raw mut table,
                std::ptr::null_mut(),
                store,
                snapshot.as_ref(),
                &raw const environment,
                u32::MAX,
                selected.as_ptr(),
                LONGHAND_DRIVE_PHASE_REMAINING,
                &raw const length,
                std::ptr::null(),
                std::ptr::null(),
                &raw mut results,
                &mut effective_color_scheme,
                true,
            );
        }
        if results.explicitly_inherited_non_inherited_style_groups != 0
            || results.uses_tree_counting_function
            || table.display_before_box_type_transformation() != old_table.display_before_box_type_transformation()
        {
            self.counters.bump(Counter::EngineComputedRecordBailDrive);
            return None;
        }
        let old_values = old_table.value_pointers();
        for &property in property_computation_order_for_phase(LONGHAND_DRIVE_PHASE_REMAINING) {
            if !is_required_driver_input(property) {
                continue;
            }
            let slot = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
            let old_value = old_values[slot];
            let new_value = table.value_pointers()[slot];
            if old_value == new_value {
                continue;
            }
            let equal = unsafe {
                match (
                    old_value.cast::<StyleValueData>().as_ref(),
                    new_value.cast::<StyleValueData>().as_ref(),
                ) {
                    (Some(old_value), Some(new_value)) => old_value == new_value,
                    _ => false,
                }
            };
            if !equal {
                self.counters.bump(Counter::EngineComputedRecordBailDrive);
                return None;
            }
            table.copy_slot_from(old_table, property);
        }
        // The group builders resolve against the same context; they report no viewport dependence
        // of their own.
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..length
        };
        Some((table, length, results.longhand_evaluations, None))
    }

    /// Drive a record through every phase: the font phase against the parent's metrics, the
    /// element's font resolved through the document's resolver, line-height and color-scheme
    /// against that font, and the remaining phase with the element facts the box-type
    /// transformation reads. Elements whose font family selects the monospace default size, and
    /// the document element, still compute in C++.
    #[allow(clippy::too_many_lines)]
    pub(super) fn engine_full_drive(
        &mut self,
        subject: DriveSubject,
        old_style_record: Option<computed::FinalStyleRecordID>,
        store: &CascadedPropertyStore,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> Option<(
        ComputedLonghandTable,
        crate::css::style_compute::FfiLengthResolutionContext,
        u32,
        Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
    )> {
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiInputLineHeightMetrics, FfiLengthResolutionContext,
            FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_COLOR_SCHEME, LONGHAND_DRIVE_PHASE_FONT,
            LONGHAND_DRIVE_PHASE_LINE_HEIGHT, LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation,
            effective_display, empty_longhand_driver_results, font_family_is_monospace, keyword,
        };
        use crate::css::table_group_builder::FfiFontGroupBuildInputs;
        use bridge::element_adjustment_fact as fact;

        let DriveSubject { parent, facts } = subject;
        let has = |bit: u32| facts & bit != 0;
        let is_document_element = has(fact::IS_DOCUMENT_ELEMENT);
        // An element with animations composes its style with their effects in C++.
        if facts & fact::HAS_ANIMATIONS != 0 {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        if self.font_resolver.is_none() {
            self.counters.bump(Counter::EngineComputedRecordBailNoEnvironment);
            return None;
        }
        if store
            .winning_declaration(prop::FONT_FAMILY)
            .is_some_and(|(value, ..)| font_family_is_monospace(unsafe { &*value.cast::<StyleValueData>() }))
        {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        }
        let old_table = match old_style_record {
            Some(old_style_record) => {
                let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecord);
                    return None;
                };
                if !view.animated_overlay.is_null() {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return None;
                }
                let Some(old_table) = (unsafe { view.longhand_table.as_ref() }) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordTable);
                    return None;
                };
                if view.dependency_flags & (1 << 2) != 0
                    || !crate::css::style_compute::active_transition_properties(old_table).is_empty()
                {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return None;
                }
                Some(old_table)
            }
            None => None,
        };
        let parent_view = match parent {
            Some(parent) => {
                let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                let Some(parent_view) = self.computed_group_sets.style_record_view(parent_record.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                if !parent_view.animated_overlay.is_null() {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return None;
                }
                Some(parent_view)
            }
            None => None,
        };
        // The document element inherits from the initial values and resolves its font against
        // the document's initial font, the way C++'s document resolution context does.
        let initial_metrics = FfiFontMetrics {
            font_size: inputs.initial_font_size,
            x_height: inputs.initial_font_x_height,
            cap_height: inputs.initial_font_cap_height,
            zero_advance: inputs.initial_font_zero_advance,
            line_height: 0.0,
        };
        let (parent_metrics, parent_font_metrics_depend_on_viewport_metrics, parent_line_height_used) =
            match &parent_view {
                Some(parent_view) => {
                    let parent_font = unsafe {
                        &*parent_view.payloads[STYLE_GROUP_INDEX_FONT]
                            .cast::<crate::css::computed_value_types::FontValues>()
                    };
                    (
                        FfiFontMetrics {
                            font_size: parent_font.font_size.to_double(),
                            x_height: drive_font_metric(parent_font.font_x_height),
                            cap_height: drive_font_metric(parent_font.font_ascent),
                            zero_advance: drive_font_metric(parent_font.font_zero_advance),
                            line_height: parent_font.line_height_used.to_double(),
                        },
                        parent_view.dependency_flags & (1 << 1) != 0,
                        parent_font.line_height_used.to_double(),
                    )
                }
                None => (initial_metrics, false, 0.0),
            };
        // C++ computes no style under a display:none ancestor.
        if old_table.is_none()
            && parent_view
                .as_ref()
                .is_some_and(|parent_view| parent_view.dependency_flags & (1 << 2) != 0)
        {
            self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return None;
        }
        // The parent's display, past any display:contents ancestor, is what the box-type
        // transformation reads.
        let mut parent_display = None;
        let mut ancestor = parent;
        while let Some(current) = ancestor {
            let Some(record) = self.computed_group_sets.assigned_style_record(current) else {
                break;
            };
            let Some(ancestor_view) = self.computed_group_sets.style_record_view(record.raw()) else {
                break;
            };
            let Some(ancestor_table) = (unsafe { ancestor_view.longhand_table.as_ref() }) else {
                break;
            };
            let display = effective_display(ancestor_table, None);
            if !display.is_contents() {
                parent_display = Some(display);
                break;
            }
            ancestor = self.tree.flat_tree_parent(current);
        }
        let snapshot = match &parent_view {
            Some(parent_view) => {
                let Some(parent_table) = (unsafe { parent_view.longhand_table.as_ref() }) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                Some(crate::css::style_compute::ParentSnapshot::new(
                    parent_table,
                    unsafe { parent_view.animated_overlay.as_ref() },
                    parent_font_metrics_depend_on_viewport_metrics,
                    parent_view.dependency_flags & (1 << 2) != 0,
                ))
            }
            None => None,
        };
        // The subject axis is the element's own writing mode when it has one, else its parent's;
        // the initial writing mode is horizontal.
        let inherited_box_payload = match old_style_record {
            Some(old_style_record) => {
                let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecord);
                    return None;
                };
                Some(view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX])
            }
            None => parent_view
                .as_ref()
                .map(|parent_view| parent_view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]),
        };
        let subject_inline_axis_is_horizontal = inherited_box_payload.is_none_or(|payload| {
            let inherited_box = unsafe { &*payload.cast::<crate::css::computed_values::InheritedBoxValues>() };
            inherited_box.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
        });
        let document_root_font_metrics = FfiFontMetrics {
            font_size: inputs.root_font_size,
            x_height: inputs.root_font_x_height,
            cap_height: inputs.root_font_cap_height,
            zero_advance: inputs.root_font_zero_advance,
            line_height: inputs.root_line_height,
        };
        let environment = FfiStyleComputationEnvironment {
            box_type_input: crate::css::style_compute::rust_box_type_transformation_input(
                facts,
                crate::css::style_compute::FfiStyleAdjustmentTarget::Element,
                parent_display.is_some(),
                parent_display.unwrap_or_else(crate::css::display::FfiDisplay::block),
            ),
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: inputs.preferred_color_scheme,
                has_document_supported_schemes: inputs.has_document_supported_schemes,
                document_supported_scheme_codes: inputs.document_supported_scheme_codes.as_ptr(),
                document_supported_scheme_count: usize::from(inputs.document_supported_scheme_count),
            },
            is_th_element: has(fact::IS_TH),
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };
        let mut resolved_viewport_relative_length = false;
        let resolved_viewport_relative_length_pointer = &raw mut resolved_viewport_relative_length;
        // The root metrics a phase resolves against: the document's, except for the document
        // element itself, whose font phase reads the initial font and whose line-height phase
        // reads its own font; its remaining phase reads the document's metrics as they stand,
        // which C++ refreshes only after computing it.
        let length_context =
            |font_metrics: FfiFontMetrics,
             font_metrics_depend_on_viewport_metrics: bool,
             root_font_metrics: FfiFontMetrics,
             root_font_metrics_depend_on_viewport_metrics: bool| FfiLengthResolutionContext {
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
                resolved_viewport_relative_length: resolved_viewport_relative_length_pointer,
            };
        let mut table = old_table.map_or_else(ComputedLonghandTable::new, ComputedLonghandTable::copied_for_drive);
        let mut results = empty_longhand_driver_results();
        let mut effective_color_scheme: i16 = -1;
        let drive = |table: &mut ComputedLonghandTable,
                     results: &mut crate::css::style_compute::FfiLonghandDriverResults,
                     effective_color_scheme: &mut i16,
                     phase: u8,
                     length: *const FfiLengthResolutionContext,
                     input_line_height_metrics: *const FfiInputLineHeightMetrics,
                     line_height_before: *const std::ffi::c_void| unsafe {
            drive_property_computation(
                std::ptr::from_mut(table),
                std::ptr::null_mut(),
                store,
                snapshot.as_ref(),
                &raw const environment,
                u32::MAX,
                std::ptr::null(),
                phase,
                length,
                input_line_height_metrics,
                line_height_before,
                std::ptr::from_mut(results),
                effective_color_scheme,
                true,
            );
        };
        let font_length = if is_document_element {
            length_context(initial_metrics, false, initial_metrics, false)
        } else {
            length_context(
                parent_metrics,
                parent_font_metrics_depend_on_viewport_metrics,
                document_root_font_metrics,
                inputs.root_font_metrics_depend_on_viewport_metrics,
            )
        };
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_FONT,
            &raw const font_length,
            std::ptr::null(),
            std::ptr::null(),
        );

        // The element's own font, resolved as the C++ font computer would for these values.
        let value_of = |table: &ComputedLonghandTable, property: u16| -> Option<&StyleValueData> {
            unsafe {
                table
                    .effective_value(None, property, true)
                    .value
                    .cast::<StyleValueData>()
                    .as_ref()
            }
        };
        // The font resolver supplies default feature and variation settings. Check computed
        // values here because non-default settings can also come from inheritance.
        for (property, default_keyword) in [
            (prop::FONT_FEATURE_SETTINGS, keyword::NORMAL),
            (prop::FONT_VARIATION_SETTINGS, keyword::NORMAL),
            (prop::FONT_VARIANT_ALTERNATES, keyword::NORMAL),
            (prop::FONT_VARIANT_CAPS, keyword::NORMAL),
            (prop::FONT_VARIANT_EAST_ASIAN, keyword::NORMAL),
            (prop::FONT_VARIANT_EMOJI, keyword::NORMAL),
            (prop::FONT_VARIANT_LIGATURES, keyword::NORMAL),
            (prop::FONT_VARIANT_NUMERIC, keyword::NORMAL),
            (prop::FONT_VARIANT_POSITION, keyword::NORMAL),
            (prop::FONT_KERNING, keyword::AUTO),
            (prop::TEXT_RENDERING, keyword::AUTO),
        ] {
            if !matches!(value_of(&table, property), Some(StyleValueData::Keyword { keyword }) if *keyword == default_keyword)
            {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return None;
            }
        }
        // The font size the element's own lengths resolve against is the C++ working set's, a
        // CSSPixels value, not the computed value's double.
        let font_size = match value_of(&table, prop::FONT_SIZE) {
            Some(StyleValueData::Length { value, unit }) if *unit == crate::css::style_compute::px_length_unit() => {
                CssPixels::nearest_value_for(*value).to_double()
            }
            _ => {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return None;
            }
        };
        let font_size_raw = CssPixels::nearest_value_for(font_size).raw_value();
        let font_family = table.effective_value(None, prop::FONT_FAMILY, true).value;
        let font_slope = match value_of(&table, prop::FONT_STYLE) {
            Some(StyleValueData::FontStyle { font_style, .. }) => match *font_style {
                crate::css::css_enums::font_style_keyword::ITALIC => 1,
                crate::css::css_enums::font_style_keyword::OBLIQUE => 2,
                _ => 0,
            },
            _ => 0,
        };
        let (font_weight, font_width) = match (value_of(&table, prop::FONT_WEIGHT), value_of(&table, prop::FONT_WIDTH))
        {
            (Some(StyleValueData::Number { value: weight }), Some(StyleValueData::Percentage { value: width })) => {
                (*weight, *width)
            }
            _ => {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return None;
            }
        };
        let font_optical_sizing = match value_of(&table, prop::FONT_OPTICAL_SIZING) {
            Some(StyleValueData::Keyword { keyword }) => {
                crate::css::css_enums::keyword_to_font_optical_sizing(*keyword).unwrap_or(0)
            }
            _ => 0,
        };
        let request = bridge::FfiFontResolutionRequest {
            font_family: font_family.cast(),
            font_size_raw,
            font_slope,
            font_weight,
            font_width,
            font_optical_sizing,
            font_environment_generation: inputs.font_environment_generation,
        };
        let Some(resolved) = self
            .font_resolver
            .as_mut()
            .and_then(|resolver| resolver.resolve(request))
        else {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        };
        let own_metrics = |line_height: f64| FfiFontMetrics {
            font_size,
            x_height: drive_font_metric(resolved.x_height),
            cap_height: drive_font_metric(resolved.ascent),
            zero_advance: drive_font_metric(resolved.zero_advance),
            line_height,
        };

        let line_height_length = if is_document_element {
            length_context(
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
            )
        } else {
            length_context(
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
                document_root_font_metrics,
                inputs.root_font_metrics_depend_on_viewport_metrics,
            )
        };
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_LINE_HEIGHT,
            &raw const line_height_length,
            std::ptr::null(),
            std::ptr::null(),
        );
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_COLOR_SCHEME,
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
        );
        effective_color_scheme = table.effective_color_scheme();

        // The used line height, as the C++ working set reads it from the computed value.
        let normal_line_height = f64::from(resolved.ascent.round() as i32 + resolved.descent.round() as i32);
        let line_height_used = |table: &ComputedLonghandTable| -> Option<f64> {
            match value_of(table, prop::LINE_HEIGHT)? {
                StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL => Some(normal_line_height),
                StyleValueData::Length { value, unit } if *unit == crate::css::style_compute::px_length_unit() => {
                    Some(CssPixels::nearest_value_for(*value).to_double())
                }
                StyleValueData::Number { value } => Some(CssPixels::nearest_value_for(value * font_size).to_double()),
                _ => None,
            }
        };
        let Some(line_height_before_adjustments) = line_height_used(&table) else {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        };
        let remaining_length = length_context(
            own_metrics(line_height_before_adjustments),
            results.font_metrics_depend_on_viewport_metrics,
            document_root_font_metrics,
            inputs.root_font_metrics_depend_on_viewport_metrics,
        );
        let input_line_height_metrics = if has(fact::CHECK_INPUT_LINE_HEIGHT) {
            FfiInputLineHeightMetrics {
                current_line_height: line_height_before_adjustments,
                minimum_line_height: normal_line_height,
            }
        } else {
            FfiInputLineHeightMetrics {
                current_line_height: 0.0,
                minimum_line_height: 0.0,
            }
        };
        let line_height_value = table.effective_value(None, prop::LINE_HEIGHT, true).value;
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_REMAINING,
            &raw const remaining_length,
            &raw const input_line_height_metrics,
            line_height_value,
        );
        if results.explicitly_inherited_non_inherited_style_groups != 0 || results.uses_tree_counting_function {
            self.counters.bump(Counter::EngineComputedRecordBailDrive);
            return None;
        }
        let Some(line_height_used_after) = line_height_used(&table) else {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        };
        let keyword_code = |property: u16, map: fn(u16) -> Option<u8>| match value_of(&table, property) {
            Some(StyleValueData::Keyword { keyword }) => map(*keyword).unwrap_or(0),
            _ => 0,
        };
        let math_depth = match value_of(&table, prop::MATH_DEPTH) {
            Some(StyleValueData::Integer { value }) => *value,
            _ => 0,
        };
        let font = FfiFontGroupBuildInputs {
            font_size_raw,
            line_height_used_raw: CssPixels::nearest_value_for(line_height_used_after).raw_value(),
            font_variant_emoji: keyword_code(
                prop::FONT_VARIANT_EMOJI,
                crate::css::css_enums::keyword_to_font_variant_emoji,
            ),
            font_ascent: resolved.ascent,
            font_descent: resolved.descent,
            font_x_height: resolved.x_height,
            font_zero_advance: resolved.zero_advance,
            first_available_font: resolved.first_available_font,
            font_cascade_list: resolved.font_cascade_list,
            font_weight,
            font_width,
            math_shift: keyword_code(prop::MATH_SHIFT, crate::css::css_enums::keyword_to_math_shift),
            math_style: keyword_code(prop::MATH_STYLE, crate::css::css_enums::keyword_to_math_style),
            math_depth,
        };
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..remaining_length
        };
        Some((table, length, results.longhand_evaluations, Some(font)))
    }
}

/// A font's pixel metric as the drive resolves font-relative units against it: the C++ length
/// resolution context carries the metrics as `CSSPixels`, so an `ex` resolves against the
/// fixed-point x-height rather than the font's raw floating-point one.
pub(super) fn drive_font_metric(value: f32) -> f64 {
    crate::css::css_pixels::CssPixels::nearest_value_for_f32(value).to_double()
}
