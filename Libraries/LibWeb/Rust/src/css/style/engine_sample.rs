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
    FfiAnimationLengthContexts, FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiHostAnimationSample,
    FfiLengthResolutionContext, FfiStyleComputationEnvironment, keyword, px_length_unit,
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
    /// element's own font. A mirror of the host's `make_computation_context_for_property(FontFamily /
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
    /// `font` is the platform font of the animated style, which a rebuilt font group needs: without
    /// it, such a build answers `Err` and rebuilds nothing. The
    /// overlay is adjusted already, as the sample's animated box-type finalization leaves it.
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
        font: Option<&FfiFontGroupBuildInputs>,
        payloads: &mut [*const std::ffi::c_void; group_index::COUNT],
    ) -> Option<Result<RebuiltOverlayGroups, NeedsHostFont>> {
        let view = self.computed_group_sets.style_record_view(style_record)?;
        payloads.copy_from_slice(SharedPayload::as_pointer_slice(view.base_payloads));
        // An overlay that animates nothing leaves the base as it is: publishing it releases the
        // element's overlay record.
        let Some(overlay) = overlay.filter(|overlay| !overlay.is_empty()) else {
            return Some(Ok(RebuiltOverlayGroups {
                groups: 0,
                every_group: true,
            }));
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
        let rebuilds_font = groups & (1 << STYLE_GROUP_INDEX_FONT) != 0;
        if rebuilds_font && font.is_none() {
            return Some(Err(NeedsHostFont));
        }
        let font_inputs = font.filter(|_| rebuilds_font);
        let own_font = match font_inputs {
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
            font: font_inputs.map_or(std::ptr::null(), std::ptr::from_ref),
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
        Some(Ok(RebuiltOverlayGroups {
            groups: rebuilt_groups,
            every_group: rebuilds_every_group,
        }))
    }
}

/// A sample of an element's animations that needs what only the host has: a container's size, an
/// adjustment of what the element's own style says, or a change beyond the element's box.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct NeedsHost;

/// The longhands an animated value of moves an adjustment the host makes after it samples, for any element.
const POST_COMPUTE_ADJUSTED_LONGHANDS: [u16; 6] = [
    prop::DISPLAY,
    prop::POSITION,
    prop::FLOAT,
    prop::OVERFLOW_X,
    prop::OVERFLOW_Y,
    prop::TEXT_ALIGN,
];

/// Whether an animated value of `property` moves an adjustment the host makes after it samples, for an element with the
/// adjustment `facts`: the line height only of an element that keeps one of its own.
fn post_compute_adjusts(property: u16, facts: u32) -> bool {
    use element_adjustment_fact::{CHECK_INPUT_LINE_HEIGHT, FORCE_LINE_HEIGHT_NORMAL};
    match property {
        prop::LINE_HEIGHT => facts & (FORCE_LINE_HEIGHT_NORMAL | CHECK_INPUT_LINE_HEIGHT) != 0,
        _ => POST_COMPUTE_ADJUSTED_LONGHANDS.contains(&property),
    }
}

/// The overlay a sample writes, which the host's working set would hold.
extern "C" fn overlay_for_mutation(overlay: *mut std::ffi::c_void) -> *mut std::ffi::c_void {
    overlay
}

extern "C" fn no_host_length_contexts(_: *mut std::ffi::c_void, _: u8, _: *mut FfiAnimationLengthContexts) {
    unreachable!("a sample without the host builds its length contexts itself");
}

impl super::StyleEngine {
    /// Samples the animations of the element `node` names over `record`, the record the host installed
    /// for it, with their timelines at `samples`, and composes them into a record no element holds,
    /// pinned for a box: what the element's box shows at that time, which no script reads. Reads only
    /// what the engine holds, so it runs wherever the engine is. `transform_reference_box` is the box a
    /// transform interpolates against, where the element has one.
    pub(crate) fn sample_at(
        &mut self,
        node: StyleNodeID,
        record: u64,
        samples: super::animations::AnimationTimelineSamples,
        transform_reference_box: Option<crate::css::css_pixels::CssPixelRect>,
    ) -> Result<super::layout_style::DerivedStyleRecord, NeedsHost> {
        use crate::css::animation as anim;

        // Each effect the host sampled last samples at the key its timing gives at these times. One that left its
        // active interval since samples nothing, and leaves none of what it animated in the host's sample.
        let effects = self.element_animation_effects(node, 0);
        let mut composed = crate::css::style_compute::SampledEffects::new();
        let mut composed_properties = smallvec::SmallVec::<[u16; 8]>::new();
        let mut ended_properties = smallvec::SmallVec::<[u16; 8]>::new();
        for effect in effects.iter().filter(|effect| effect.keyframes.len() >= 2) {
            let Some(timing) = &effect.timing else {
                continue;
            };
            let properties = effect
                .keyframes
                .iter()
                .flat_map(|keyframe| effect.declarations_of(keyframe))
                .map(|declaration| declaration.property_id);
            match timing.key_at(samples).ok_or(NeedsHost)? {
                Some(current_key) => {
                    composed.push(anim::FfiSampledAnimationEffect {
                        effect: anim::FfiAnimationPreparationEffect {
                            identity: effect.identity,
                            generation: effect.generation,
                        },
                        current_key,
                    });
                    composed_properties.extend(properties);
                }
                None => ended_properties.extend(properties),
            }
        }

        let view = self.computed_group_sets.style_record_view(record).ok_or(NeedsHost)?;
        let table = view.longhand_table;
        // SAFETY: The record holds its table and overlay, and the host's install pins the record.
        let mut overlay = unsafe { view.animated_overlay.as_ref() }.cloned().unwrap_or_default();
        for &property in ended_properties
            .iter()
            .filter(|property| !composed_properties.contains(property))
        {
            overlay.remove_animated(property);
        }
        let outcome = self
            .sample_over_record(node, record, &mut overlay, None, composed, transform_reference_box)?
            .outcome;
        if outcome == crate::css::style_compute::FfiHostAnimationSampleOutcome::Cleared {
            overlay = AnimatedOverlay::default();
        }

        // What the host adjusts after it samples, an animated color scheme, and an inherited value the
        // element's children hold as of the host's sample, are the host's to compose.
        let has_children = self.tree.first_element_child(node).is_some();
        let facts = self.computed_group_sets.adjustment_facts(node);
        if overlay.entries().iter().any(|entry| {
            entry.post_compute_adjustment
                || post_compute_adjusts(entry.property, facts)
                || entry.property == prop::COLOR_SCHEME
                || (has_children && crate::css::property_metadata::property_is_inherited(entry.property))
        }) {
            return Err(NeedsHost);
        }
        // SAFETY: As above.
        let table = unsafe { table.as_ref() }.ok_or(NeedsHost)?;
        let used_color_scheme = u8::try_from(table.effective_color_scheme()).map_err(|_| NeedsHost)?;
        let mut payloads = [std::ptr::null(); group_index::COUNT];
        let mut build = |engine: &Self, font: Option<&FfiFontGroupBuildInputs>| {
            engine
                .build_animation_overlay_payloads(
                    node,
                    crate::css::cascaded_properties::NO_PSEUDO_ELEMENT,
                    record,
                    table,
                    Some(&overlay),
                    used_color_scheme,
                    table.display_before_box_type_transformation(),
                    font,
                    &mut payloads,
                )
                .ok_or(NeedsHost)
        };
        // A sample that moves the font asks the document's font resolver for it, as the host's sample does.
        let rebuilt = match build(self, None)? {
            Err(NeedsHostFont) => {
                let font = self
                    .animated_font_group_inputs(node, table, &overlay)
                    .ok_or(NeedsHost)?;
                build(self, Some(&font))?.map_err(|NeedsHostFont| NeedsHost)?
            }
            Ok(rebuilt) => rebuilt,
        };
        // What the sample changes beyond its box is the host's to show: the visual contexts above all, which the
        // compositor has from the host's frames.
        if !self.animation_sample_stays_in_its_box(node, record, &overlay, SharedPayload::from_pointer_slice(&payloads))
        {
            release_rebuilt_overlay_payloads(&payloads, rebuilt.groups);
            return Err(NeedsHost);
        }
        let sampled = super::bridge::publish_computed_groups_from_inputs(
            self,
            0,
            u8::MAX,
            SharedPayload::from_pointer_slice(&payloads),
            super::computed::ENGINE_INHERITED_GROUP_COUNT,
            0,
            false,
            0,
            0,
            std::ptr::null(),
            &[],
            Some(table),
            std::ptr::null(),
        )
        .new_style_record;
        release_rebuilt_overlay_payloads(&payloads, rebuilt.groups);
        Ok(super::layout_style::DerivedStyleRecord::pin(self, sampled))
    }

    /// Samples `composed` onto `overlay` over `record`, a record the engine holds for the element `node`
    /// names, as far as the engine goes without the host, reading the effects `fresh` describes, or the
    /// element's where it is `None`.
    pub(crate) fn sample_over_record(
        &mut self,
        node: StyleNodeID,
        record: u64,
        overlay: &mut AnimatedOverlay,
        fresh: Option<&[super::effect_descriptions::PublishedEffect]>,
        composed: crate::css::style_compute::SampledEffects,
        transform_reference_box: Option<crate::css::css_pixels::CssPixelRect>,
    ) -> Result<crate::css::style_compute::FfiHostAnimationSampleResult, NeedsHost> {
        let table = self
            .computed_group_sets
            .style_record_view(record)
            .ok_or(NeedsHost)?
            .longhand_table;
        // A keyframe substitutes against the custom properties of the element and of its parent, as the host's
        // sample does, from the stores the engine holds for their environments throughout the sample.
        let inheritance_parent = self.tree.inheritance_parent(node);
        let custom_property_environments = [Some(node), inheritance_parent].map(|node| {
            node.and_then(|node| self.computed_group_sets.custom_property_environment_identity(node))
                .unwrap_or(0)
        });
        let [store, inheritance_store] = custom_property_environments.map(|identity| {
            self.custom_property_environments
                .store(identity)
                .unwrap_or(std::ptr::null())
        });
        let inheritance_parent_style_record = inheritance_parent
            .and_then(|parent| self.held_style_records.get(&parent).copied())
            .unwrap_or(0);
        let inputs = &self.document_style_computation_inputs;
        let environment = FfiStyleComputationEnvironment {
            // SAFETY: As the host's value-initialized input, which an animation sample does not read.
            box_type_input: unsafe { std::mem::zeroed() },
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: inputs.preferred_color_scheme,
                has_document_supported_schemes: inputs.has_document_supported_schemes,
                document_supported_scheme_codes: inputs.document_supported_scheme_codes.as_ptr(),
                document_supported_scheme_count: usize::from(inputs.document_supported_scheme_count),
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
        let overlay_pointer = std::ptr::from_mut(overlay).cast::<std::ffi::c_void>();
        let input = FfiHostAnimationSample {
            host: std::ptr::null(),
            style_node: node.raw(),
            pseudo_kind: crate::css::cascaded_properties::NO_PSEUDO_ELEMENT,
            effects: std::ptr::null(),
            effect_count: 0,
            longhand_table: table.cast_mut().cast_const().cast(),
            animated_overlay: overlay_pointer.cast_const(),
            style_record: record,
            custom_property_store: store,
            // A sample that animates a custom property is the host's, so none composes over the element's own.
            base_custom_property_store: store,
            inheritance_custom_property_store: inheritance_store,
            element_declares_own_custom_properties: false,
            custom_property_environments,
            inheritance_parent_style_record,
            environment: &raw const environment,
            element_box_slot: crate::layout::node_data::NodeSlotId::INVALID.index,
            callback_context: overlay_pointer,
            prepare_overlay_for_mutation: overlay_for_mutation,
            length_contexts: no_host_length_contexts,
        };
        // SAFETY: Everything `input` names lives until the sample returns.
        unsafe {
            crate::css::style_compute::sample_without_host(&input, self, fresh, composed, transform_reference_box)
        }
    }

    /// The elements whose style a size query or container-relative unit decided below `container`, whose content box a
    /// layout moved.
    pub(crate) fn size_container_query_dependents(&self, container: StyleNodeID) -> Vec<StyleNodeID> {
        self.state.retained.size_container_query_dependents(container).0
    }

    /// The element whose restyle shows what the new size of a container decides for `node`: `node` itself, or the
    /// element whose boxes a clock frame built with `node`'s among them, which shows them all at once. None for an
    /// element below a box a clock frame took away, which shows nothing.
    pub(crate) fn size_query_restyle_target(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if self.tick_shown.is_empty() {
            return Some(node);
        }
        let mut target = node;
        for ancestor in std::iter::successors(self.tree.flat_tree_parent(node), |&ancestor| {
            self.tree.flat_tree_parent(ancestor)
        }) {
            match self.tick_shown.get(ancestor).map(|shown| shown.boxes) {
                Some(ShownBoxes::TakenAway) => return None,
                Some(ShownBoxes::Built) => target = ancestor,
                _ => {}
            }
        }
        Some(target)
    }

    /// What `node` computes to against the boxes as the last layout committed them, which the engine derives for this
    /// read alone, as it does for a read of the element's style: what the element published and the record the host
    /// installed stay as they were. `boxed` says whether the element has a box now, which an element hidden without
    /// one keeps, as the host computes no style for it. Records that move the boxes the element generates, its
    /// pseudo-elements', its own, which it loses, or the ones it gains, are shown to the clock frame's tree build in
    /// place of the host's (see [`TickShownRecords`]), as are the ones a box a clock frame built for it moves to.
    /// Where the move from the host's record starts a transition or an animation, or reaches a box a build cannot
    /// build again by itself, the host computes it.
    pub(crate) fn restyle_size_query_dependent(
        &mut self,
        node: StyleNodeID,
        boxed: bool,
    ) -> Result<DependentRestyle, NeedsHost> {
        let host_record = self.computed_group_sets.assigned_style_record(node).ok_or(NeedsHost)?;
        if self.record_holds_an_animation_overlay(host_record) {
            return Err(NeedsHost);
        }
        let record = self.derive_record(node, bridge::FfiRecordDemand::ElementRead)?;
        let restyled = super::computed::FinalStyleRecordID::from_raw(record.style_record).ok_or(NeedsHost)?;
        // The box a clock frame built shows the records it was built from, whatever the host's say.
        let built = self.tick_shown.get(node).map(|shown| shown.boxes) == Some(ShownBoxes::Built);
        if built {
            if !self.record_generates_a_box(restyled) {
                return self.take_box_away(node, restyled);
            }
            let moved = self.show_built_boxes(node, ShownBoxes::Built, restyled, &record)?;
            return Ok(if moved {
                DependentRestyle::BuiltBoxesMove
            } else {
                DependentRestyle::Unmoved
            });
        }
        if !boxed {
            if !self.record_generates_a_box(restyled) {
                return Ok(DependentRestyle::Unmoved);
            }
            let parent = self.box_rebuild_parent(node).ok_or(NeedsHost)?;
            self.show_built_boxes(node, ShownBoxes::Built, restyled, &record)?;
            return Ok(DependentRestyle::GainsABox { parent });
        }
        let element_moves = restyled != host_record;
        if element_moves && self.record_declares_transitions(restyled) {
            return Err(NeedsHost);
        }
        if !self.record_generates_a_box(restyled) {
            return self.take_box_away(node, restyled);
        }
        let pseudo_elements = self.pseudo_records_to_show(node, &record)?;
        if element_moves && !self.restyle_stays_in_its_box(node, host_record.raw(), restyled.raw()) {
            return Err(NeedsHost);
        }
        let in_box = super::layout_style::DerivedStyleRecord::pin(self, restyled.raw());
        let Some((pseudo_present, pseudo)) = pseudo_elements else {
            // What an earlier clock frame showed for the pseudo-elements stands beside the element's record.
            if let Some(&shown) = self.tick_shown.get(node)
                && shown.element != restyled
            {
                let shown = ShownRecords {
                    element: restyled,
                    ..shown
                };
                self.state.retained.show_for_tick(node, shown);
            }
            return Ok(DependentRestyle::InBox(in_box));
        };
        let shown = ShownRecords {
            boxes: ShownBoxes::Restyled,
            element: restyled,
            pseudo_present,
            pseudo,
        };
        self.state.retained.show_for_tick(node, shown);
        Ok(DependentRestyle::PseudoElementsMove(in_box))
    }

    /// The record a read-only `demand` derives for `node`.
    fn derive_record(
        &mut self,
        node: StyleNodeID,
        demand: bridge::FfiRecordDemand,
    ) -> Result<super::publication::DemandedEngineRecord, NeedsHost> {
        match self.answer_record_demand(node, super::publication::RecordDemand::Element(demand)) {
            Ok(super::publication::RecordDemandAnswer::Record { record, .. }) => Ok(record),
            _ => Err(NeedsHost),
        }
    }

    /// Shows `restyled`, which generates no box, for `node`, whose box the clock frame takes away.
    fn take_box_away(
        &mut self,
        node: StyleNodeID,
        restyled: super::computed::FinalStyleRecordID,
    ) -> Result<DependentRestyle, NeedsHost> {
        let parent = self.box_rebuild_parent(node).ok_or(NeedsHost)?;
        self.state.retained.show_for_tick(node, ShownRecords::hidden(restyled));
        Ok(DependentRestyle::LosesItsBox { parent })
    }

    /// Shows `element`, the record a read derived for `node`, `record`, in the box a clock frame builds for it, and the
    /// record each element below it computes to against it in theirs, as `boxes` built. Answers whether any moved from
    /// what the clock showed.
    fn show_built_boxes(
        &mut self,
        node: StyleNodeID,
        boxes: ShownBoxes,
        element: super::computed::FinalStyleRecordID,
        record: &super::publication::DemandedEngineRecord,
    ) -> Result<bool, NeedsHost> {
        self.check_builds_plainly(node, element, record)?;
        let shown = ShownRecords {
            boxes,
            element,
            pseudo_present: record.pseudo_records_present,
            pseudo: record.pseudo_records,
        };
        let moved = self.state.retained.show_for_tick(node, shown);
        if self.tree.first_element_child(node).is_none() || self.record_is_in_display_none_subtree(element.raw()) {
            return Ok(moved);
        }
        // The elements below read their parent's record as the one assigned to it, which it is while they are read: in
        // place of the host's, which an element below one without a box may not have yet, or not at all when the flush
        // left it unstyled in a display:none subtree.
        let host = self
            .computed_group_sets
            .assigned_style_record(node)
            .unwrap_or(super::computed::FinalStyleRecordID::NONE);
        let replaced = if host == element {
            None
        } else {
            let replaced = self
                .computed_group_sets
                .assign_engine_computed_record(node, host, element);
            Some(replaced.ok_or(NeedsHost)?)
        };
        let below = self.show_built_children(node);
        if let Some(replaced) = replaced {
            self.computed_group_sets
                .revert_engine_computed_record(node, element, replaced);
        }
        Ok(below? || moved)
    }

    /// Shows the records the element children of `parent` compute to against the one assigned to it, in the boxes a
    /// clock frame builds for them. Answers whether any moved from what the clock showed.
    fn show_built_children(&mut self, parent: StyleNodeID) -> Result<bool, NeedsHost> {
        let mut moved = false;
        let mut child = self.tree.first_element_child(parent);
        while let Some(node) = child {
            child = self.tree.next_element_sibling(node);
            let record = self.derive_record(node, bridge::FfiRecordDemand::ElementReadAgainstParent)?;
            let element = super::computed::FinalStyleRecordID::from_raw(record.style_record).ok_or(NeedsHost)?;
            moved |= self.show_built_boxes(node, ShownBoxes::BuiltBelow, element, &record)?;
        }
        Ok(moved)
    }

    /// Refuses an element whose boxes only the host builds: one whose type asks for more than its display does, one in
    /// a shadow tree or holding one, a container, a list item, and one that starts a transition or an animation, or
    /// whose counters, quotes or generated boxes reach beyond its own box. `element` is the record the clock shows for
    /// it, and `record` what the read that derived it settled.
    fn check_builds_plainly(
        &self,
        node: StyleNodeID,
        element: super::computed::FinalStyleRecordID,
        record: &super::publication::DemandedEngineRecord,
    ) -> Result<(), NeedsHost> {
        use super::publication::pseudo_kind::{AFTER, BACKDROP, BEFORE, FIRST_LETTER, FIRST_LINE, MARKER};
        use element_adjustment_fact::{
            HAS_ANIMATIONS, IS_BUTTON, IS_MATHML, IS_SHADOW_HOST_PSEUDO_ELEMENT, IS_SVG_ELEMENT, IS_TABLE,
            RENDERED_IN_TOP_LAYER,
        };
        let typed = IS_SVG_ELEMENT
            | IS_MATHML
            | IS_TABLE
            | IS_BUTTON
            | RENDERED_IN_TOP_LAYER
            | IS_SHADOW_HOST_PSEUDO_ELEMENT
            | HAS_ANIMATIONS;
        let is_container =
            |record: super::computed::FinalStyleRecordID| self.container_query_input_row(record.raw(), false).is_some();
        let generates_pseudo_boxes = [AFTER, BACKDROP, BEFORE, FIRST_LETTER, FIRST_LINE, MARKER]
            .iter()
            .any(|&kind| {
                record.pseudo_records_present & (1 << kind) != 0
                    || self.computed_group_sets.pseudo_style_record(node, kind).is_some()
            });
        let Some(view) = self.computed_group_sets.style_record_view(element.raw()) else {
            return Err(NeedsHost);
        };
        let values =
            crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(view.payloads));
        let plain = self.element_box_kind(node) == bridge::ElementBoxKind::FromDisplay
            && self.computed_group_sets.adjustment_facts(node) & typed == 0
            && self.tree.shadow_root_of(node).is_none()
            && self.tree.assigned_slot_of(node).is_none()
            && !is_container(element)
            && !self
                .computed_group_sets
                .assigned_style_record(node)
                .is_some_and(is_container)
            && !values.display().is_list_item()
            && !values.display().is_contents()
            && !values.affects_generated_content_state()
            && !generates_pseudo_boxes
            && !self.record_declares_transitions(element)
            && !self.record_declares_animations(element.raw());
        plain.then_some(()).ok_or(NeedsHost)
    }

    /// The pseudo-element records `node` shows with `record`, the record a read derived for it, where they move what
    /// its boxes show now: the ones the read settled, and the host's for those an earlier clock frame showed another
    /// of. Only a `::before` and an `::after` are built again by themselves, and only the host starts their
    /// transitions and numbers the counters and quotes after them.
    fn pseudo_records_to_show(
        &self,
        node: StyleNodeID,
        record: &super::publication::DemandedEngineRecord,
    ) -> Result<Option<(u16, [u64; bridge::PSEUDO_RECORD_SLOTS])>, NeedsHost> {
        use super::publication::pseudo_kind::{AFTER, BEFORE};
        let shown = self.tick_shown.get(node);
        let shown_present = shown.map_or(0, |shown| shown.pseudo_present);
        let present = record.pseudo_records_present | shown_present;
        let mut pseudo = [0; bridge::PSEUDO_RECORD_SLOTS];
        let mut moved = false;
        for kind in (0..bridge::PSEUDO_RECORD_SLOTS).filter(|&kind| present & (1 << kind) != 0) {
            let host = self
                .computed_group_sets
                .pseudo_style_record(node, kind as u8)
                .map_or(0, super::computed::FinalStyleRecordID::raw);
            pseudo[kind] = match record.pseudo_records_present & (1 << kind) {
                0 => host,
                _ => record.pseudo_records[kind],
            };
            let showing = match (shown, shown_present & (1 << kind)) {
                (Some(shown), bit) if bit != 0 => shown.pseudo[kind],
                _ => host,
            };
            if pseudo[kind] == showing {
                continue;
            }
            let regenerates = kind == usize::from(BEFORE) || kind == usize::from(AFTER);
            let reaches_beyond = |record: u64| {
                super::computed::FinalStyleRecordID::from_raw(record).is_some_and(|record| {
                    self.record_declares_transitions(record) || self.record_affects_generated_content_state(record)
                })
            };
            if !regenerates || reaches_beyond(pseudo[kind]) || reaches_beyond(showing) {
                return Err(NeedsHost);
            }
            moved = true;
        }
        Ok(moved.then_some((present, pseudo)))
    }

    /// The element whose box the host builds again for `node`, which a clock frame took the box of: its parent, which
    /// places `node`'s box among its children wherever the host's record gives it one. None where the host places the
    /// box otherwise: the root, the body, an SVG element and a top layer member, and a box that is not its parent's
    /// child alone, that of a slotted element or of a child of a shadow root or a shadow host.
    fn box_rebuild_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        use element_adjustment_fact::{
            IS_DOCUMENT_ELEMENT, IS_HTML_BODY_ELEMENT, IS_SVG_ELEMENT, RENDERED_IN_TOP_LAYER,
        };
        let parent = self.tree.parent(node)?;
        let placed_otherwise = IS_DOCUMENT_ELEMENT | IS_HTML_BODY_ELEMENT | IS_SVG_ELEMENT | RENDERED_IN_TOP_LAYER;
        (self.computed_group_sets.adjustment_facts(node) & placed_otherwise == 0
            && self.tree.assigned_slot_of(node).is_none()
            && self.tree.shadow_root_of(parent).is_none()
            && self.computed_group_sets.assigned_style_record(parent).is_some())
        .then_some(parent)
    }

    /// Shows `record`, the sample a clock frame showed in the box of `node`, in the box a build makes for it again.
    pub(crate) fn show_sample_in_rebuilt_box(&mut self, node: StyleNodeID, record: u64) {
        let Some(record) = super::computed::FinalStyleRecordID::from_raw(record) else {
            return;
        };
        let shown = match self.tick_shown.get(node) {
            Some(&shown) => ShownRecords {
                element: record,
                ..shown
            },
            None => ShownRecords {
                boxes: ShownBoxes::Restyled,
                element: record,
                pseudo_present: 0,
                pseudo: [0; bridge::PSEUDO_RECORD_SLOTS],
            },
        };
        self.state.retained.show_for_tick(node, shown);
    }

    /// Lends the engine's published reads the records `shown`, which a clock frame shows in the boxes it builds.
    pub(crate) fn lend_tick_shown(&mut self, shown: TickShownRecords) {
        debug_assert!(
            self.tick_shown.0.is_empty(),
            "one clock frame at a time shows its records"
        );
        self.state.retained.tick_shown = shown;
    }

    /// Takes back the records [`Self::lend_tick_shown`] lent, with what the clock frame showed since.
    pub(crate) fn take_tick_shown(&mut self) -> TickShownRecords {
        std::mem::take(&mut self.state.retained.tick_shown)
    }

    /// Lets go of the records clock frames showed, whose boxes the host builds again from its own.
    pub(crate) fn release_tick_shown(&mut self, shown: TickShownRecords) {
        for (_, shown) in shown.0 {
            shown.unpin(&mut self.state.retained.computed_group_sets);
        }
    }

    /// Whether `record` moves a counter or a quote depth, which the boxes after its own read.
    fn record_affects_generated_content_state(&self, record: super::computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_none_or(|view| {
                crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(
                    view.payloads,
                ))
                .affects_generated_content_state()
            })
    }

    /// Whether an element with `record` generates a box: it is neither under a `display: none` ancestor nor itself
    /// `display: none` or `display: contents`.
    fn record_generates_a_box(&self, record: super::computed::FinalStyleRecordID) -> bool {
        let Some(view) = self.computed_group_sets.style_record_view(record.raw()) else {
            return true;
        };
        let display =
            crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(view.payloads))
                .display();
        view.dependency_flags & super::computed::IN_DISPLAY_NONE_SUBTREE == 0
            && !display.is_none()
            && !display.is_contents()
    }

    /// What the font group of `node`'s record is built from over `overlay`, with the font the document's font resolver
    /// resolves for it, which the engine asks the resolver for where it has not yet. None where the document published
    /// no resolver.
    fn animated_font_group_inputs(
        &mut self,
        node: StyleNodeID,
        table: &ComputedLonghandTable,
        overlay: &AnimatedOverlay,
    ) -> Option<FfiFontGroupBuildInputs> {
        let request = font_resolution_request(table, Some(overlay), &self.document_style_computation_inputs, || {
            self.font_feature_values_scope(node)
        });
        let lookup = |engine: &Self| engine.font_resolution.as_ref()?.lookup(request);
        let resolved = match lookup(self) {
            Some(resolved) => resolved,
            None => {
                if self.host.font_resolver.is_none() || self.font_resolution.is_none() {
                    return None;
                }
                let Self { counters, state } = self;
                state.refill_font_request(node, super::font_resolution::FontRequest::new(request), counters);
                lookup(self)?
            }
        };
        let line_height_used = used_line_height(table, Some(overlay), &request, &resolved);
        Some(font_group_build_inputs(
            table,
            Some(overlay),
            &request,
            line_height_used,
            &resolved,
        ))
    }
}

/// How a clock frame shows what the new size of a container decides for an element below it.
pub(crate) enum DependentRestyle {
    /// What the element's boxes show stands.
    Unmoved,
    /// A record that moves nothing beyond the element's box, which the frame shows in it.
    InBox(super::layout_style::DerivedStyleRecord),
    /// Records that move the boxes of the element's `::before` or `::after`, which the frame builds again from them,
    /// beside a record that moves nothing beyond the element's box, which it shows in it.
    PseudoElementsMove(super::layout_style::DerivedStyleRecord),
    /// A record that generates no box, which the frame takes the element's box away for, and which `parent`'s box
    /// places among its children wherever the host's record gives the element one.
    LosesItsBox { parent: StyleNodeID },
    /// Records for the element and every element below it that give it a box, which the frame builds from them and
    /// inserts among `parent`'s children.
    GainsABox { parent: StyleNodeID },
    /// Records that move the boxes a clock frame built for the element and below it, which the frame builds again
    /// from them.
    BuiltBoxesMove,
}

/// The records the render clock's frames show in place of the ones the host installed, for the elements whose boxes
/// they built again: what the size a frame laid a container out at decides for them. A frame lends them to the
/// engine's published reads, which a tree build builds boxes from, and takes them back as it ends, so the host never
/// reads them. Each
/// record is pinned until the host lets go of them as it builds those boxes again from its own.
#[derive(Default)]
pub(crate) struct TickShownRecords(Vec<(StyleNodeID, ShownRecords)>);

impl TickShownRecords {
    pub(crate) fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    fn get(&self, node: StyleNodeID) -> Option<&ShownRecords> {
        self.0
            .iter()
            .find_map(|(shown_node, shown)| (*shown_node == node).then_some(shown))
    }

    /// The record shown for `node`'s element, where a clock frame shows one.
    pub(super) fn element(&self, node: StyleNodeID) -> Option<super::computed::FinalStyleRecordID> {
        self.get(node).map(|shown| shown.element)
    }

    /// The record shown for `node`'s pseudo-element `kind`, where a clock frame shows one, and none where it
    /// shows the pseudo-element has none.
    fn pseudo(&self, node: StyleNodeID, kind: u8) -> Option<Option<super::computed::FinalStyleRecordID>> {
        let shown = self.get(node)?;
        let slot = usize::from(kind);
        (slot < bridge::PSEUDO_RECORD_SLOTS && shown.pseudo_present & (1 << slot) != 0)
            .then(|| super::computed::FinalStyleRecordID::from_raw(shown.pseudo[slot]))
    }
}

/// What a clock frame shows of the boxes an element generates in place of the host's.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ShownBoxes {
    /// The element keeps a box, which shows the record, and whose `::before` and `::after` show the ones shown.
    Restyled,
    /// A clock frame took the element's box away.
    TakenAway,
    /// A clock frame built the element's box, and every box below it.
    Built,
    /// A clock frame built the element's box with those of a [`ShownBoxes::Built`] ancestor.
    BuiltBelow,
}

/// What a clock frame shows for one element.
#[derive(Clone, Copy, PartialEq, Eq)]
struct ShownRecords {
    boxes: ShownBoxes,
    element: super::computed::FinalStyleRecordID,
    /// The pseudo-element kinds whose records the frame shows, a bit per kind; a shown kind holding zero has none.
    pseudo_present: u16,
    pseudo: [u64; bridge::PSEUDO_RECORD_SLOTS],
}

impl ShownRecords {
    /// What a clock frame shows for an element whose box it takes away.
    fn hidden(element: super::computed::FinalStyleRecordID) -> Self {
        Self {
            boxes: ShownBoxes::TakenAway,
            element,
            pseudo_present: 0,
            pseudo: [0; bridge::PSEUDO_RECORD_SLOTS],
        }
    }

    fn records(&self) -> impl Iterator<Item = u64> + '_ {
        let pseudo = (0..bridge::PSEUDO_RECORD_SLOTS)
            .filter(|&kind| self.pseudo_present & (1 << kind) != 0)
            .map(|kind| self.pseudo[kind]);
        std::iter::once(self.element.raw())
            .chain(pseudo)
            .filter(|&record| record != 0)
    }

    fn unpin(&self, computed_group_sets: &mut super::computed::ComputedGroupSets) {
        self.records()
            .for_each(|record| computed_group_sets.unpin_style_record(record));
    }
}

impl RetainedState {
    /// Shows `shown` for `node` in place of what a clock frame showed for it before, answering whether that moved.
    fn show_for_tick(&mut self, node: StyleNodeID, shown: ShownRecords) -> bool {
        let entries = &mut self.tick_shown.0;
        let previous = entries.iter_mut().find(|(shown_node, _)| *shown_node == node);
        if previous.as_ref().is_some_and(|(_, previous)| *previous == shown) {
            return false;
        }
        shown
            .records()
            .for_each(|record| self.computed_group_sets.pin_style_record(record));
        match previous {
            Some((_, previous)) => std::mem::replace(previous, shown).unpin(&mut self.computed_group_sets),
            None => entries.push((node, shown)),
        }
        true
    }

    /// The element record the boxes built from `node` take: the one a clock frame shows, or the one the engine
    /// assigned.
    pub(super) fn published_element_record(&self, node: StyleNodeID) -> Option<super::computed::FinalStyleRecordID> {
        self.tick_shown
            .element(node)
            .or_else(|| self.computed_group_sets.assigned_style_record(node))
    }

    /// The record the boxes built for `node`'s pseudo-element `kind` take: the one a clock frame shows, or the
    /// one the element holds.
    pub(super) fn published_pseudo_record(
        &self,
        node: StyleNodeID,
        kind: u8,
    ) -> Option<super::computed::FinalStyleRecordID> {
        self.tick_shown
            .pseudo(node, kind)
            .unwrap_or_else(|| self.computed_group_sets.pseudo_style_record(node, kind))
    }
}

/// A build of overlay payloads that rebuilds the font group, asked without the host's font.
pub(crate) struct NeedsHostFont;

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
