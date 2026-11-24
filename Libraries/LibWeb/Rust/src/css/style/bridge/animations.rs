/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

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
/// `installed` must view a record the host holds live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_animation_overlay_changed(
    installed: &FfiStyleRecordView,
    animated_overlay: Option<&AnimatedOverlay>,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let (longhands, old_overlay) = unsafe {
        (
            installed
                .longhands()
                .expect("the record of an element carries its longhands"),
            installed.animated_overlay(),
        )
    };
    crate::css::style::style_invalidation::animation_overlay_changed(longhands, old_overlay, animated_overlay)
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
    pub record: *const crate::css::style::pipeline::Record,
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

/// Whether the engine assigns a record to the pseudo-element `pseudo_kind` of `node`'s element, for its animations to
/// compose over. It assigns none to a pseudo-element that is not rendered, whose record a read of its style alone
/// holds.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_assigns_pseudo_element_record(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    pseudo_kind: u8,
) -> bool {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return false;
    };
    with_engine(read, host, |engine| {
        engine
            .computed_group_sets
            .holds_an_installed_record(crate::css::style::computed::ComputedStyleTarget::new(node, pseudo_kind))
    })
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
        engine.is_svg_graphics_element(node),
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
    engine
        .retained
        .live_counters
        .note_animated_sample(node, rebuilt.every_group);
    Ok(FfiAnimationOverlayPublication {
        invalidation,
        publication,
        record: engine
            .computed_group_sets
            .host_record(publication.new_style_record)
            .map_or(std::ptr::null(), |record| std::sync::Arc::into_raw(record.clone())),
        rebuilt_every_group: rebuilt.every_group,
    })
}
