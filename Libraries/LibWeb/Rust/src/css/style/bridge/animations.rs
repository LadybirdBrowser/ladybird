/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// How one animation effect's description travels across the boundary: the ranges of the flat
/// buffers that belong to it.
#[repr(C)]
pub struct FfiPublishedAnimationEffect {
    pub identity: u64,
    pub generation: u64,
    /// The effect belongs to a CSS transition, which the interpolation treats differently.
    pub is_transition: bool,
    /// The effect's keyframes come from a style sheet, which their URLs resolve against: its base
    /// URL is the range of the base URL buffer below.
    pub has_resource_context: bool,
    pub resource_context_is_origin_clean: bool,
    pub first_keyframe: u32,
    pub keyframe_count: u32,
    pub base_url_offset: u32,
    pub base_url_length: u32,
    /// A transition's reversing-adjusted start value, or null for any other effect.
    pub reversing_adjusted_start_value: *const c_void,
    /// A transition's reversing shortening factor.
    pub reversing_shortening_factor: f64,
}

/// The easing function a published keyframe spells out.
#[repr(u8)]
#[derive(Clone, Copy)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiPublishedEasingKind {
    Linear,
    CubicBezier,
    Steps,
}

/// One keyframe of a published effect: its offset on the scale the host keys keyframes by, its
/// easing spelled out, its composite operation and the ranges of what it declares.
#[repr(C)]
pub struct FfiPublishedAnimationKeyframe {
    pub key: i64,
    pub easing_kind: FfiPublishedEasingKind,
    pub step_position: u8,
    pub interval_count: i32,
    pub x1: f64,
    pub y1: f64,
    pub x2: f64,
    pub y2: f64,
    pub first_linear_point: u32,
    pub linear_point_count: u32,
    /// The keyframe's own easing where it still has to be substituted against the element being
    /// sampled, or null.
    pub easing_value: *const c_void,
    /// The keyframe's composite operation, with `auto` already the effect's own.
    pub composite: crate::css::animation::FfiCompositeOperation,
    pub first_declaration: u32,
    pub declaration_count: u32,
    pub first_custom_declaration: u32,
    pub custom_declaration_count: u32,
}

/// One control point of a published `linear()` easing.
#[repr(C)]
pub struct FfiPublishedLinearEasingPoint {
    pub input: f64,
    pub output: f64,
}

/// One longhand a published keyframe declares. A null value stands for the element's own value,
/// which a keyframe the host synthesized holds and which is not known until the element is
/// sampled.
#[repr(C)]
pub struct FfiPublishedAnimationDeclaration {
    pub property_id: u16,
    pub value: *const c_void,
}

/// One custom property a published keyframe declares, by the raw representation of its name. A
/// null value stands for the element's own value of it.
#[repr(C)]
pub struct FfiPublishedAnimationCustomDeclaration {
    pub name: usize,
    pub value: *const c_void,
}

/// The version of one animation effect the host is about to sample.
#[repr(C)]
pub struct FfiAnimationEffectVersion {
    pub identity: u64,
    pub generation: u64,
}

/// Describe the effects one of an element's animation lists holds, in composite order, for the
/// style engine to sample them from. The host retains every value the effects name and streams
/// the description to the engine.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, every buffer must hold the count
/// it is given, and every value and custom-property name the buffers name must be live for the
/// duration of the call.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn style_engine_set_element_animation_effect_descriptions(
    host: &DocumentHost,
    node: u32,
    slot: u32,
    effects: *const FfiPublishedAnimationEffect,
    effect_count: usize,
    keyframes: *const FfiPublishedAnimationKeyframe,
    keyframe_count: usize,
    declarations: *const FfiPublishedAnimationDeclaration,
    declaration_count: usize,
    custom_declarations: *const FfiPublishedAnimationCustomDeclaration,
    custom_declaration_count: usize,
    linear_points: *const FfiPublishedLinearEasingPoint,
    linear_point_count: usize,
    base_url_bytes: *const u8,
    base_url_byte_count: usize,
) {
    // NB: The slot of an element's own animations is u8::MAX, whose upper bits a u8 parameter could not promise.
    let slot = slot as u8;
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    let effects = unsafe {
        crate::css::style::effect_descriptions::PublishedEffectBuffers {
            effects: ak::slice_from_raw_parts(effects, effect_count),
            keyframes: ak::slice_from_raw_parts(keyframes, keyframe_count),
            declarations: ak::slice_from_raw_parts(declarations, declaration_count),
            custom_declarations: ak::slice_from_raw_parts(custom_declarations, custom_declaration_count),
            linear_points: ak::slice_from_raw_parts(linear_points, linear_point_count),
            base_url_bytes: ak::slice_from_raw_parts(base_url_bytes, base_url_byte_count),
        }
        .effects()
    };
    host.engine_memo().described.borrow_mut().follow(node, slot, &effects);
    host.queue_change(crate::render_state::ArenaChange::Engine(
        crate::css::style::engine_calls::EngineWrite::AnimationEffectDescriptions { node, slot, effects },
    ));
}

/// Notes what the `attr()`s of the value `value`, which an animation effect's keyframes hold, can read, answering
/// whether they can read a name none could before.
///
/// # Safety
/// `value` must be a live style value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_note_attr_names_read_by_value(host: &DocumentHost, value: *const c_void) -> bool {
    // SAFETY: Guaranteed by the caller.
    let value = unsafe { &*value.cast::<crate::css::style_value::StyleValueData>() };
    let before = crate::css::style::host_atoms::attr_names_read_generation(host);
    crate::css::style::host_atoms::note_attr_names_read_by(host, std::iter::once(value));
    crate::css::style::host_atoms::attr_names_read_generation(host) != before
}

/// Whether the engine describes one of an element's animation lists as holding exactly these
/// versions of its effects, in this order, so that the host need not describe them again. Only
/// the host describes them, so it knows without asking.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_describes_animation_effects(
    host: &DocumentHost,
    node: u32,
    slot: u32,
    versions: ak::Span<'_, FfiAnimationEffectVersion>,
) -> bool {
    StyleNodeID::from_raw(node).is_some_and(|node| {
        host.engine_memo()
            .described
            .borrow()
            .describe(node, slot as u8, versions.as_slice())
    })
}

/// Publish already bound immutable selector inputs, after all host interning has finished.
pub(crate) fn publish_style_rule(
    engine: &mut StyleEngine,
    sheet: u32,
    before_rule: u32,
    compiled: &SelectorListRef,
    bound_scope: &BoundScopeChain,
) -> u32 {
    if sheet == 0 || compiled.is_empty() {
        return 0;
    }
    let before = match before_rule {
        0 => None,
        id => Some(RuleID(id - 1)),
    };
    let rule = engine.add_style_rule_in_scope(SheetID(sheet - 1), before, compiled, Some(bound_scope));
    rule.0 + 1
}

/// Replace selectors using immutable compilation inputs, preserving rule identity.
pub(crate) fn publish_style_rule_selectors(
    engine: &mut StyleEngine,
    rule: u32,
    compiled: &SelectorListRef,
    bound_scope: &BoundScopeChain,
) {
    if rule == 0 || compiled.is_empty() {
        return;
    }
    engine.replace_style_rule_selectors(RuleID(rule - 1), compiled, Some(bound_scope));
}
/// Whether a candidate animation overlay changes any effective value of the record `installed` views, in place of the
/// record's own overlay. The host reads it from the record it installed, as the engine would.
///
/// # Safety
/// `installed` must view a record the host holds live, and `animated_overlay` must be null or live, for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_animation_overlay_changed(
    installed: &FfiStyleRecordView,
    animated_overlay: *const c_void,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let (longhands, old_overlay, new_overlay) = unsafe {
        (
            installed
                .longhands()
                .expect("the record of an element carries its longhands"),
            installed.animated_overlay(),
            animated_overlay.cast::<AnimatedOverlay>().as_ref(),
        )
    };
    crate::css::style::style_invalidation::animation_overlay_changed(longhands, old_overlay, new_overlay)
}

/// What the host hands over to have an element's sampled animation overlay composed over the record it installed and
/// published.
#[repr(C)]
pub struct FfiAnimationOverlayPublicationInput {
    pub style_node: u32,
    pub pseudo_kind: u8,
    /// The record the element installed, which the overlay was sampled on and composes over.
    pub style_record: u64,
    /// The style record whose longhands the overlay was sampled over.
    pub record: *const c_void,
    pub animated_overlay: *const c_void,
    /// The identity of the overlay, or 0 where it animates nothing, which releases the target's overlay record.
    pub animation_overlay_identity: u64,
    pub used_color_scheme: u8,
    pub display_before_box_type_transformation_raw: u32,
    pub is_document_element: bool,
    /// What a target the engine holds no overlay slot for is published again whole with, beside the installed record's
    /// base: the custom-property environment the target holds, with its store.
    pub custom_property_environment: u64,
    pub custom_property_store: *const c_void,
    pub callback_context: *mut c_void,
    /// Writes the animated style's platform font, a `ComputedValuesFFI::FfiFontGroupBuildInputs`;
    /// asked only where the font group is rebuilt.
    pub font_group_inputs: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

/// What publishing an element's sampled animation overlay answers: what replacing the installed record with it damages,
/// the record the target held and the one it holds now, with the view of the latter, which is missing where the engine
/// holds no record the overlay was sampled on.
#[repr(C)]
pub struct FfiAnimationOverlayPublication {
    pub invalidation: FfiAnimationInvalidation,
    pub publication: FfiStyleRecordDelta,
    /// The record the host holds for the record the target holds now, with a reference the host takes over: a later
    /// publication of the same call can retire the record.
    pub record: *const c_void,
    /// Whether the overlay named a value the groups could not be told from, or nothing at all.
    pub rebuilt_every_group: bool,
}

impl FfiAnimationOverlayPublication {
    fn missing() -> Self {
        Self {
            invalidation: FfiAnimationInvalidation::default(),
            publication: FfiStyleRecordDelta::default(),
            record: std::ptr::null(),
            rebuilt_every_group: false,
        }
    }
}

/// Composes each element's sampled animation overlay over the record it installed, rebuilding only the groups the
/// overlay writes (see `RetainedState::build_animation_overlay_payloads`), compares it with that record, and publishes
/// it as the target's record, writing what it answers to the same index of `out`: every element in one call of the
/// engine. An element whose overlay rebuilds the font group is published again in a call of its own: resolving the
/// animated font may read the engine, so the host resolves it between the two. The host keeps the view of each record
/// it answers, which it installs next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `inputs` and `out` must point to `count` entries, and
/// everything an input points to must be live for the call, its table non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_sampled_animation_overlays(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    inputs: *const FfiAnimationOverlayPublicationInput,
    count: usize,
    out: *mut FfiAnimationOverlayPublication,
) {
    use crate::css::table_group_builder::FfiFontGroupBuildInputs;

    // SAFETY: Guaranteed by the caller.
    let (inputs, out) = unsafe {
        (
            ak::slice_from_raw_parts(inputs, count),
            ak::slice_from_raw_parts_mut(out, count),
        )
    };
    with_engine(read, host, |engine| {
        for (input, out) in inputs.iter().zip(out.iter_mut()) {
            // SAFETY: Guaranteed by the caller.
            *out = unsafe { publish_sampled_animation_overlay(engine, input, None) }.unwrap_or_else(
                |crate::css::style::engine_sample::NeedsHostFont| FfiAnimationOverlayPublication::missing(),
            );
        }
    });
    for (input, out) in inputs.iter().zip(out.iter_mut()) {
        if out.record.is_null() {
            // A publication the first call could not make, for want of the font or of a record, is tried again with
            // the font.
            let mut font = std::mem::MaybeUninit::<FfiFontGroupBuildInputs>::uninit();
            // SAFETY: Guaranteed by the caller. The host writes the whole font.
            let font = unsafe {
                (input.font_group_inputs)(input.callback_context, font.as_mut_ptr().cast());
                font.assume_init()
            };
            // SAFETY: Guaranteed by the caller.
            *out = with_engine(read, host, |engine| unsafe {
                publish_sampled_animation_overlay(engine, input, Some(&font))
            })
            .unwrap_or_else(|crate::css::style::engine_sample::NeedsHostFont| {
                FfiAnimationOverlayPublication::missing()
            });
        }
        if !out.record.is_null() {
            host.engine_memo()
                .views
                .set(out.publication.new_style_record, out.record);
        }
    }
}

/// [`style_engine_publish_sampled_animation_overlays`]'s call of `engine` for one element, which answers `Err` where it
/// needs the animated font, `font`, it was not given.
///
/// # Safety
/// As for [`style_engine_publish_sampled_animation_overlays`], for one input.
pub(in crate::css::style) unsafe fn publish_sampled_animation_overlay(
    engine: &mut StyleEngine,
    input: &FfiAnimationOverlayPublicationInput,
    font: Option<&crate::css::table_group_builder::FfiFontGroupBuildInputs>,
) -> Result<FfiAnimationOverlayPublication, crate::css::style::engine_sample::NeedsHostFont> {
    use crate::css::table_group_builder::group_index;

    let Some(node) = StyleNodeID::from_raw(input.style_node) else {
        return Ok(FfiAnimationOverlayPublication::missing());
    };
    let Some(installed_style_record) = crate::css::style::computed::FinalStyleRecordID::from_raw(input.style_record)
    else {
        return Ok(FfiAnimationOverlayPublication::missing());
    };
    // NB: style styles an element-backed pseudo-element as the element of the shadow tree that
    //     backs it, so the animations of the pseudo-element compose over that element's record.
    let (node, pseudo_kind) = match input.pseudo_kind {
        kind @ FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND..=LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND => {
            let element = crate::css::cascaded_properties::NO_PSEUDO_ELEMENT;
            let backing = engine
                .backing_elements(node, 1 << kind)
                .find(|&backing| {
                    engine.computed_group_sets.assigned_final_style_record(
                        crate::css::style::computed::ComputedStyleTarget::new(backing, element),
                    ) == Some(installed_style_record)
                })
                .expect(
                    "the engine assigns the record an element-backed pseudo-element installed to its backing element",
                );
            (backing, element)
        }
        kind => (node, kind),
    };
    // SAFETY: Guaranteed by the caller.
    let (longhands, overlay) = unsafe {
        (
            crate::css::style::ViewLonghands::of_raw_record(input.record),
            input.animated_overlay.cast::<AnimatedOverlay>().as_ref(),
        )
    };
    let mut payloads = [std::ptr::null(); group_index::COUNT];
    let Some(old_record) = engine.computed_group_sets.host_record(input.style_record).cloned() else {
        return Ok(FfiAnimationOverlayPublication::missing());
    };
    let old_view = crate::css::style::StyleRecordView::of_host_record(&old_record);
    let rebuilt = engine.build_animation_overlay_payloads(
        old_view,
        longhands,
        overlay,
        input.used_color_scheme,
        input.display_before_box_type_transformation_raw,
        font,
        &mut payloads,
    )?;
    // SAFETY: The rebuild wrote a live payload of each group.
    let groups = &unsafe { crate::css::computed_values::StyleGroups::retained(&payloads, rebuilt.bits) };
    // Compared before the publication, which may release the installed record: nothing else keeps a pseudo-element's.
    let invalidation = engine.compare_animation_overlay(
        old_view,
        input.animated_overlay.cast(),
        groups,
        input.is_document_element,
    );
    let (animated_overlay, overlay_payloads) = match input.animation_overlay_identity {
        0 => (std::ptr::null(), None),
        _ => (input.animated_overlay, Some(groups)),
    };
    let publication = match engine.publish_animation_overlay_impl(
        crate::css::style::computed::ComputedStyleTarget::new(node, pseudo_kind),
        input.animation_overlay_identity,
        HostShared::new(animated_overlay).cast(),
        overlay_payloads,
    ) {
        Some(publication) => FfiStyleRecordDelta {
            old_style_record: publication.previous_style_record.raw(),
            new_style_record: publication.style_record.raw(),
        },
        // NB: The engine assigns every pseudo-element it styles the record it installed, so a target it holds no
        //     assignment for has no style to compose over.
        None => {
            crate::css::style::engine_sample::release_rebuilt_overlay_payloads(&payloads, rebuilt.groups);
            return Ok(FfiAnimationOverlayPublication::missing());
        }
    };
    crate::css::style::engine_sample::release_rebuilt_overlay_payloads(&payloads, rebuilt.groups);
    engine.retained.live_counters.note_animated_sample(rebuilt.every_group);
    Ok(FfiAnimationOverlayPublication {
        invalidation,
        publication,
        record: engine
            .computed_group_sets
            .host_record(publication.new_style_record)
            .map_or(std::ptr::null(), |record| {
                std::sync::Arc::into_raw(record.clone()).cast()
            }),
        rebuilt_every_group: rebuilt.every_group,
    })
}
