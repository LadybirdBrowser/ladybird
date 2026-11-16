/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which animations of a document the compositor runs, which it replaces, and which are throttled, as a rendering
//! update decides it, and the wakeups that keep the main thread's view of them current. The host answers what the
//! layout, paint and observer state of the document says of their targets.

use super::animation::{AnimationCell, AnimationHandle};
use super::composite_order::composite_order;
use super::document_animations::{DocumentAnimationSetKind, DocumentAnimations};
use super::effect::{AnimationEffectCell, AnimationEffectId, CompositorAnimationStateAccess, EffectFlag, EffectHandle};
use super::ffi::{FfiKeepAlive, FfiTreeOrder, host_tree_order};
use super::host_timing::{FfiAnimationKind, FfiAnimationTimingState, FfiCompositorAnchorKind};
use super::keyframes::{KeyframeSet, KeyframeValue};
use super::throttling::{can_skip_per_frame_animation_tick, can_skip_per_frame_style_update};
use super::timing::{AnimationEffectPhase, AnimationPlayState, TimeValueType};
use crate::css::property_metadata::property_id;
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::{
    FfiCompositorAnimationBuildOutcome, FfiCompositorAnimationHost, FfiCompositorAnimationRequest,
    FfiCompositorAnimationTiming, FfiVisualAnimationTargetKind, TARGETED_TRANSFORM_PROPERTY_ROTATE,
    TARGETED_TRANSFORM_PROPERTY_SCALE, TARGETED_TRANSFORM_PROPERTY_TRANSFORM, TARGETED_TRANSFORM_PROPERTY_TRANSLATE,
};
use crate::painting::visual_animation_builder::{
    CompositorAnimationEffectState, Host, Request, effect_only_translates_horizontally, effect_transform_preserves_axes,
};
use crate::render_state::DocumentHost;
use libcompositing_rust::easing::Easing;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

/// The target of an effect as the host's `DOM::AbstractElement`: the host's `DOM::Element`, and the host's
/// `PseudoElement` where `has_pseudo_element`.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct FfiCompositorTarget {
    pub element: NonNull<c_void>,
    pub has_pseudo_element: bool,
    pub pseudo_element: u8,
}

/// A visual context node the host forces on a box for a compositor animation.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiCompositorFrameKind {
    EffectsLayer,
    BackgroundColor,
}

/// What the host prepares a compositor animation of an effect for: the target kind, the target and its box, the
/// properties the effect targets, and the instant it is anchored at. `checked` asks the host to check that the
/// compositor can run the animation for the box too.
#[repr(C)]
pub struct FfiCompositorAnimationPreparation {
    pub target_kind: u8,
    pub target: FfiCompositorTarget,
    pub layout_node: NodeSlotId,
    pub targeted_transform_properties: u8,
    pub checked: bool,
}

/// What the host proves of a paint-only effect, as far as its properties go.
#[repr(C)]
pub struct FfiOffscreenPaintQuery {
    pub targets_only_opacity: bool,
    pub targets_opacity: bool,
    /// Whether nothing the effect animates paints outside the border box of the target.
    pub paint_stays_within_border_box: bool,
    /// Whether the effect animates only properties that SVG descendants of the target cannot paint elsewhere.
    pub can_include_svg_descendants: bool,
    /// Whether the box `layout_node` or the element `element` of an ancestor of the target has an animation that can
    /// move or expand its paint, with `context`.
    pub context: *const c_void,
    pub ancestor_animation_moves_paint:
        unsafe extern "C" fn(context: *const c_void, layout_node: NodeSlotId, element: *mut c_void) -> bool,
}

/// A target of an in-effect transform animation, which an observation of a transformed subtree tells apart.
#[repr(C)]
pub struct FfiTransformAnimationTarget {
    pub element: NonNull<c_void>,
    pub effect: AnimationEffectId,
}

/// What the host reads of the transform animations of a pass as it finds whether a transform animation affects an
/// intersection observation.
#[repr(C)]
pub struct FfiTransformObservationQuery {
    pub effect: AnimationEffectId,
    pub only_translates_horizontally: bool,
    pub transform_targets: *const FfiTransformAnimationTarget,
    pub transform_target_count: usize,
    /// Whether the transform animations of the box `layout_node` keep its axes in place, with `context`.
    pub context: *const c_void,
    pub transform_animations_preserve_axes:
        unsafe extern "C" fn(context: *const c_void, layout_node: NodeSlotId) -> bool,
}

/// What the host answers and does for an update of a document's compositor animations, with `context`.
#[repr(C)]
pub struct FfiCompositorUpdateHost {
    pub context: *mut c_void,
    pub document_host: *const DocumentHost,
    pub force_dark_applies: bool,
    pub has_throttled_animation_style_update: bool,
    pub time_origin: f64,
    pub has_last_animation_frame_timestamp: bool,
    pub last_animation_frame_timestamp: f64,
    pub keep_alive: FfiKeepAlive,
    pub tree_order: FfiTreeOrder,
    pub layout_node: unsafe extern "C" fn(context: *mut c_void, target: FfiCompositorTarget) -> NodeSlotId,
    /// Notes the box of a target before the pass changes what it retains, and that the pass retains it.
    pub note_target_layout_node: unsafe extern "C" fn(context: *mut c_void, layout_node: NodeSlotId),
    pub note_retained_layout_node: unsafe extern "C" fn(context: *mut c_void, layout_node: NodeSlotId),
    /// Whether an observer tracks the visibility of the element or of something in it.
    pub visibility_is_observed: unsafe extern "C" fn(context: *mut c_void, element: NonNull<c_void>) -> bool,
    /// Fills in what the host knows of the request and the host the keyframes' values resolve with, answering whether
    /// the compositor can run the animation.
    pub prepare_compositor_animation: unsafe extern "C" fn(
        context: *mut c_void,
        preparation: &FfiCompositorAnimationPreparation,
        request: *mut c_void,
        keyframe_host: *mut c_void,
    ) -> bool,
    pub paint_is_offscreen:
        unsafe extern "C" fn(context: *mut c_void, target: FfiCompositorTarget, query: &FfiOffscreenPaintQuery) -> bool,
    pub transform_affects_intersection_observation: unsafe extern "C" fn(
        context: *mut c_void,
        animated_target: NonNull<c_void>,
        query: &FfiTransformObservationQuery,
        requires_main_thread_sampling: &mut bool,
    ) -> bool,
    pub transform_subtree_is_clipped_outside_viewport:
        unsafe extern "C" fn(context: *mut c_void, element: NonNull<c_void>) -> bool,
    pub background_color_can_be_compositor_animated:
        unsafe extern "C" fn(context: *mut c_void, layout_node: NodeSlotId) -> bool,
    /// Forces the node on the box where it has none yet, answering whether it did.
    pub request_frame:
        unsafe extern "C" fn(context: *mut c_void, layout_node: NodeSlotId, kind: FfiCompositorFrameKind) -> bool,
    /// Keeps the node the pass forced on the box, and whether the last pass kept it.
    pub keep_frame: unsafe extern "C" fn(context: *mut c_void, layout_node: NodeSlotId, kind: FfiCompositorFrameKind),
    pub frame_was_kept:
        unsafe extern "C" fn(context: *mut c_void, layout_node: NodeSlotId, kind: FfiCompositorFrameKind) -> bool,
    /// Maps the viewport again, through the visual contexts forcing a node changed.
    pub refresh_viewport_mapping: unsafe extern "C" fn(context: *mut c_void),
    pub note_throttled_animation_style_update: unsafe extern "C" fn(context: *mut c_void),
}

/// What an update of a document's compositor animations leaves the host to do.
#[repr(C)]
#[derive(Default)]
pub struct FfiCompositorUpdateOutcome {
    pub has_observation_relevant_compositor_animation: bool,
    pub requested_withdrawn_effect_sample: bool,
    pub has_wakeup_delay: bool,
    pub wakeup_delay: f64,
}

/// What a wakeup of the compositor animations of a document leaves the host to do.
#[repr(C)]
#[derive(Default)]
pub struct FfiCompositorWakeupOutcome {
    pub has_next_wakeup_delay: bool,
    pub next_wakeup_delay: f64,
    pub reached_wakeup: bool,
    pub reached_compositor_active_start: bool,
}

unsafe extern "C" {
    fn web_animation_set_needs_animated_style_update(effect: *const c_void, target: NonNull<c_void>);
}

/// Asks the next style update for an observation sample of `effect`.
pub(crate) fn request_observation_sample(effect: &AnimationEffectCell) {
    if effect.borrow_mut().set_flag(EffectFlag::NeedsObservationSample, true) {
        set_needs_animated_style_update(effect);
    }
}

/// Has the next animated style update of the document of the target of `effect` sample it.
pub(crate) fn set_needs_animated_style_update(effect: &AnimationEffectCell) {
    let target = effect.borrow().target_element();
    if let Some(target) = target {
        // SAFETY: The effect keeps its target alive.
        unsafe { web_animation_set_needs_animated_style_update(std::ptr::from_ref(effect).cast(), target) };
    }
}

pub(crate) fn is_transform_family(property: u16) -> bool {
    matches!(
        property,
        property_id::TRANSFORM | property_id::TRANSLATE | property_id::ROTATE | property_id::SCALE
    )
}

fn minimum(delay: &mut Option<f64>, candidate: f64) {
    if delay.is_none_or(|delay| candidate < delay) {
        *delay = Some(candidate);
    }
}

/// What the pass reads of an effect's record once.
struct EffectFacts {
    id: AnimationEffectId,
    target: Option<FfiCompositorTarget>,
    keyframes: Option<Arc<KeyframeSet>>,
    composite_is_replace: bool,
    may_run_on_the_compositor: bool,
}

impl EffectFacts {
    fn of(effect: &AnimationEffectCell) -> Self {
        let record = effect.borrow();
        Self {
            id: record.id(),
            target: record.target_element().map(|element| {
                let pseudo_element = record.pseudo_element();
                FfiCompositorTarget {
                    element,
                    has_pseudo_element: pseudo_element.is_some(),
                    pseudo_element: pseudo_element.map_or(0, |pseudo_element| pseudo_element.kind),
                }
            }),
            keyframes: record.keyframes().cloned(),
            composite_is_replace: record.composite() == crate::css::animation::FfiCompositeOperation::Replace,
            may_run_on_the_compositor: record.may_run_on_the_compositor(),
        }
    }

    fn properties(&self) -> &[u16] {
        self.keyframes
            .as_ref()
            .map_or(&[][..], |keyframes| keyframes.target_properties())
    }

    fn has_custom_properties(&self) -> bool {
        self.keyframes
            .as_ref()
            .is_some_and(|keyframes| !keyframes.target_custom_properties().is_empty())
    }

    fn has_target_properties(&self) -> bool {
        !self.properties().is_empty() || self.has_custom_properties()
    }

    fn targets(&self, property: u16) -> bool {
        self.properties().contains(&property)
    }

    fn targets_transform(&self) -> bool {
        self.properties().iter().copied().any(is_transform_family)
    }

    fn targeted_transform_properties(&self) -> u8 {
        let mut properties = 0;
        for (property, flag) in [
            (property_id::TRANSLATE, TARGETED_TRANSFORM_PROPERTY_TRANSLATE),
            (property_id::ROTATE, TARGETED_TRANSFORM_PROPERTY_ROTATE),
            (property_id::SCALE, TARGETED_TRANSFORM_PROPERTY_SCALE),
            (property_id::TRANSFORM, TARGETED_TRANSFORM_PROPERTY_TRANSFORM),
        ] {
            if self.targets(property) {
                properties |= flag;
            }
        }
        properties
    }
}

/// The effects competing for one property group of a target, and the one whose output the compositor could run.
struct CompetingPropertyEffects {
    winner: Option<(EffectHandle, AnimationHandle)>,
    all_effects_use_replace: bool,
    effect_count: u32,
}

impl Default for CompetingPropertyEffects {
    fn default() -> Self {
        Self {
            winner: None,
            all_effects_use_replace: true,
            effect_count: 0,
        }
    }
}

impl CompetingPropertyEffects {
    fn is_won_by(&self, effect: &EffectHandle) -> bool {
        self.winner.as_ref().is_some_and(|(winner, _)| winner.ptr_eq(effect))
    }
}

#[derive(Default)]
struct CompetingEffects {
    opacity: CompetingPropertyEffects,
    background_color: CompetingPropertyEffects,
    filter: CompetingPropertyEffects,
    transform: CompetingPropertyEffects,
}

/// One associated animation of the pass with its associated effect.
struct Candidate {
    animation: AnimationHandle,
    effect: EffectHandle,
    facts: EffectFacts,
    was_throttled: bool,
    was_published: bool,
}

/// The compositor animation of `kind` of an effect for the box `layout_node` of `target` the pass asks the host to
/// prepare, anchored at `timing_anchor` as the monotonic time and the local time there, where the host is `checked`
/// it can run it.
#[derive(Clone, Copy)]
struct CompositorAnimationAsk {
    target: FfiCompositorTarget,
    layout_node: NodeSlotId,
    kind: FfiVisualAnimationTargetKind,
    checked: bool,
    timing_anchor: (f64, f64),
}

/// What a pass knows as it goes, which the host's queries read back through its callbacks.
struct Pass<'a> {
    host: &'a FfiCompositorUpdateHost,
    competing_effects: Vec<(FfiCompositorTarget, CompetingEffects)>,
    competing_index: HashMap<FfiCompositorTarget, usize>,
    in_effect_transform_effects_by_layout_node: HashMap<NodeSlotId, Vec<(EffectHandle, AnimationHandle)>>,
    animated_transform_preserves_axes: RefCell<HashMap<AnimationEffectId, bool>>,
}

fn existing_state(effect: &AnimationEffectCell) -> Option<*mut CompositorAnimationEffectState> {
    effect
        .borrow_mut()
        .compositor_animation_state(CompositorAnimationStateAccess::Existing)
        .map(std::ptr::from_mut)
}

fn state_of(effect: &AnimationEffectCell) -> *mut CompositorAnimationEffectState {
    std::ptr::from_mut(
        effect
            .borrow_mut()
            .compositor_animation_state(CompositorAnimationStateAccess::CreateIfMissing)
            .expect("the state is made where missing"),
    )
}

fn has_retained_compositor_animations(effect: &AnimationEffectCell) -> bool {
    // SAFETY: The effect keeps its state for as long as it lives.
    existing_state(effect).is_some_and(|state| unsafe { (*state).has_retained() })
}

impl Pass<'_> {
    fn layout_node(&self, target: FfiCompositorTarget) -> Option<NodeSlotId> {
        // SAFETY: The host answers synchronously.
        let slot = unsafe { (self.host.layout_node)(self.host.context, target) };
        (slot != NodeSlotId::INVALID).then_some(slot)
    }

    fn competing(&self, target: &FfiCompositorTarget) -> Option<&CompetingEffects> {
        self.competing_index
            .get(target)
            .map(|&index| &self.competing_effects[index].1)
    }

    fn composite_order_is_before(&self, a: &AnimationCell, b: &AnimationCell) -> bool {
        if std::ptr::eq(a, b) {
            return false;
        }
        composite_order(&a.borrow(), &b.borrow(), &mut host_tree_order(self.host.tree_order)).is_lt()
    }

    /// Prepares the request for a compositor animation of `kind` of the effect for the box `layout_node`, answering it
    /// with the host its keyframes resolve with, where the host finds the compositor can run it.
    fn prepare(
        &self,
        effect: &AnimationEffectCell,
        animation: &AnimationCell,
        facts: &EffectFacts,
        ask: CompositorAnimationAsk,
        default_easing: &Easing,
    ) -> Option<(FfiCompositorAnimationRequest, FfiCompositorAnimationHost)> {
        let CompositorAnimationAsk {
            target,
            layout_node,
            kind,
            checked,
            timing_anchor,
        } = ask;
        let effect_timing = effect.borrow().timing();
        let mut request = FfiCompositorAnimationRequest {
            target_kind: kind,
            layout_node,
            target_inherited_ui_values: std::ptr::null(),
            target_inherited_text_values: std::ptr::null(),
            timing: FfiCompositorAnimationTiming {
                monotonic_time_at_anchor_ns: (timing_anchor.0 * 1_000_000.0) as i64,
                local_time_at_anchor_ms: timing_anchor.1,
                playback_rate: animation.borrow().playback().playback_rate,
                effect: effect_timing,
                easing: default_easing.descriptor(),
            },
            keyframe_set: facts
                .keyframes
                .as_ref()
                .map_or(std::ptr::null(), |keyframes| Arc::as_ptr(keyframes).cast()),
            effect_composite_is_replace: facts.composite_is_replace,
            default_easing: default_easing.descriptor(),
            target_style_generation: 0,
            style_environment_version: 0,
            reference_box_width: 0.0,
            reference_box_height: 0.0,
            device_pixels_per_css_pixel: 1.0,
            targeted_transform_properties: facts.targeted_transform_properties(),
            targets_only_transform: facts.properties() == [property_id::TRANSFORM] && !facts.has_custom_properties(),
        };
        let mut keyframe_host = std::mem::MaybeUninit::<FfiCompositorAnimationHost>::uninit();
        let preparation = FfiCompositorAnimationPreparation {
            target_kind: kind as u8,
            target,
            layout_node,
            targeted_transform_properties: request.targeted_transform_properties,
            checked,
        };
        // SAFETY: The host answers synchronously, and fills in the keyframe host where it answers true.
        unsafe {
            (self.host.prepare_compositor_animation)(
                self.host.context,
                &preparation,
                std::ptr::from_mut(&mut request).cast(),
                keyframe_host.as_mut_ptr().cast(),
            )
            .then(|| (request, keyframe_host.assume_init()))
        }
    }

    /// Builds the compositor animation of `kind` for the effect, which it keeps pending, where the compositor can run
    /// it.
    fn build(
        &self,
        effect: &EffectHandle,
        facts: &EffectFacts,
        kind: FfiVisualAnimationTargetKind,
    ) -> FfiCompositorAnimationBuildOutcome {
        let Some(animation) = effect.borrow().associated_animation() else {
            return FfiCompositorAnimationBuildOutcome::default();
        };
        // NB: The record decides whether the compositor may run the effect as far as its timing goes, and where its
        //     animation is anchored.
        let state = animation.borrow().timing_state(None);
        if state.compositor_anchor == FfiCompositorAnchorKind::None || !facts.has_target_properties() {
            return FfiCompositorAnimationBuildOutcome::default();
        }
        let targets_kind = match kind {
            FfiVisualAnimationTargetKind::Opacity => facts.targets(property_id::OPACITY),
            FfiVisualAnimationTargetKind::BackgroundColor => facts.targets(property_id::BACKGROUND_COLOR),
            FfiVisualAnimationTargetKind::Filter => facts.targets(property_id::FILTER),
            FfiVisualAnimationTargetKind::Transform => facts.targets_transform(),
        };
        if !targets_kind || !facts.may_run_on_the_compositor {
            return FfiCompositorAnimationBuildOutcome::default();
        }
        let Some(target) = facts.target else {
            return FfiCompositorAnimationBuildOutcome::default();
        };
        let monotonic_time_at_anchor = if state.compositor_anchor == FfiCompositorAnchorKind::OriginRelativeTime {
            state.compositor_anchor_origin_relative_time + self.host.time_origin
        } else if self.host.has_last_animation_frame_timestamp {
            self.host.last_animation_frame_timestamp + self.host.time_origin
        } else {
            return FfiCompositorAnimationBuildOutcome::default();
        };
        let Some(layout_node) = self.layout_node(target) else {
            return FfiCompositorAnimationBuildOutcome::default();
        };
        if facts
            .keyframes
            .as_ref()
            .is_none_or(|keyframes| keyframes.keyframes().len() < 2)
        {
            return FfiCompositorAnimationBuildOutcome::default();
        }
        let default_easing = default_easing_of(&animation, &state);
        let Some((request, keyframe_host)) = self.prepare(
            effect,
            &animation,
            facts,
            CompositorAnimationAsk {
                target,
                layout_node,
                kind,
                checked: true,
                timing_anchor: (monotonic_time_at_anchor, state.compositor_anchor_local_time),
            },
            &default_easing,
        ) else {
            return FfiCompositorAnimationBuildOutcome::default();
        };
        let state = state_of(effect);
        // SAFETY: The effect keeps its state for as long as it lives, the document host is live for the pass, and the
        //         request and the keyframe host live for the call.
        unsafe {
            crate::painting::ffi::build_compositor_animation_effect(
                &mut *state,
                &*self.host.document_host,
                request,
                effect,
                &animation,
                &keyframe_host,
            )
        }
    }

    /// Answers `question` of the transform keyframes of the effect for the box `layout_node`.
    fn ask_transform_keyframes(
        &self,
        effect: &EffectHandle,
        facts: &EffectFacts,
        layout_node: NodeSlotId,
        question: fn(&Request, &Host) -> bool,
    ) -> bool {
        let Some(animation) = effect.borrow().associated_animation() else {
            return false;
        };
        let Some(target) = facts.target else {
            return false;
        };
        if facts.keyframes.is_none() {
            return false;
        }
        let state = animation.borrow().timing_state(None);
        let default_easing = default_easing_of(&animation, &state);
        let Some((request, keyframe_host)) = self.prepare(
            effect,
            &animation,
            facts,
            CompositorAnimationAsk {
                target,
                layout_node,
                kind: FfiVisualAnimationTargetKind::Transform,
                checked: false,
                timing_anchor: (0.0, 0.0),
            },
            &default_easing,
        ) else {
            return false;
        };
        // SAFETY: The request lives for the call.
        let request = unsafe { Request::new(&request) };
        question(&request, &Host::new(&keyframe_host))
    }

    fn animated_transform_preserves_axes(&self, effect: &EffectHandle, layout_node: NodeSlotId) -> bool {
        let facts = EffectFacts::of(effect);
        if let Some(&cached) = self.animated_transform_preserves_axes.borrow().get(&facts.id) {
            return cached;
        }
        let preserves_axes = if facts.targets(property_id::ROTATE) {
            false
        } else if !facts.targets(property_id::TRANSFORM) {
            true
        } else {
            self.ask_transform_keyframes(effect, &facts, layout_node, effect_transform_preserves_axes)
        };
        self.animated_transform_preserves_axes
            .borrow_mut()
            .insert(facts.id, preserves_axes);
        preserves_axes
    }

    /// Whether a paint-only effect cannot show, as its target is offscreen.
    fn paint_only_effect_is_offscreen(&self, facts: &EffectFacts, target: FfiCompositorTarget) -> bool {
        // NB: Only properties whose changes are confined to painting are eligible. In particular, color can
        //     affect SVG stroke geometry, and transforms can affect layout. Filters require an ancestor clip.
        if facts.has_custom_properties() {
            return false;
        }
        let mut paint_stays_within_border_box = true;
        for &property in facts.properties() {
            match property {
                property_id::BACKGROUND_COLOR
                // NB: Keyframes containing var() retain the shorthand until substitution.
                | property_id::BACKGROUND_POSITION
                | property_id::BACKGROUND_POSITION_X
                | property_id::BACKGROUND_POSITION_Y
                | property_id::BACKGROUND_SIZE
                | property_id::BACKGROUND_REPEAT
                | property_id::BACKGROUND_ORIGIN
                | property_id::BACKGROUND_CLIP
                | property_id::BACKGROUND_BLEND_MODE
                | property_id::BORDER_TOP_COLOR
                | property_id::BORDER_RIGHT_COLOR
                | property_id::BORDER_BOTTOM_COLOR
                | property_id::BORDER_LEFT_COLOR => {}
                property_id::OPACITY
                | property_id::FILTER
                | property_id::BOX_SHADOW
                | property_id::OUTLINE_COLOR
                | property_id::OUTLINE_OFFSET
                | property_id::OUTLINE_STYLE
                | property_id::OUTLINE_WIDTH
                | property_id::TEXT_DECORATION_COLOR
                | property_id::TEXT_DECORATION_STYLE
                | property_id::TEXT_DECORATION_THICKNESS => paint_stays_within_border_box = false,
                _ => return false,
            }
        }
        let query = FfiOffscreenPaintQuery {
            targets_only_opacity: facts
                .properties()
                .iter()
                .all(|&property| property == property_id::OPACITY),
            targets_opacity: facts.targets(property_id::OPACITY),
            paint_stays_within_border_box,
            can_include_svg_descendants: facts
                .properties()
                .iter()
                .all(|&property| matches!(property, property_id::OPACITY | property_id::FILTER)),
            context: std::ptr::from_ref(self).cast(),
            ancestor_animation_moves_paint,
        };
        // SAFETY: The host answers synchronously.
        unsafe { (self.host.paint_is_offscreen)(self.host.context, target, &query) }
    }
}

unsafe extern "C" fn ancestor_animation_moves_paint(
    context: *const c_void,
    layout_node: NodeSlotId,
    element: *mut c_void,
) -> bool {
    // SAFETY: The pass hands the host itself as the context, for the call it makes.
    let pass = unsafe { &*context.cast::<Pass>() };
    if pass
        .in_effect_transform_effects_by_layout_node
        .contains_key(&layout_node)
    {
        return true;
    }
    let Some(element) = NonNull::new(element) else {
        return false;
    };
    let target = FfiCompositorTarget {
        element,
        has_pseudo_element: false,
        pseudo_element: 0,
    };
    pass.competing(&target)
        .is_some_and(|effects| effects.filter.winner.is_some())
}

unsafe extern "C" fn transform_animations_preserve_axes(context: *const c_void, layout_node: NodeSlotId) -> bool {
    // SAFETY: The pass hands the host itself as the context, for the call it makes.
    let pass = unsafe { &*context.cast::<Pass>() };
    let Some(effects) = pass.in_effect_transform_effects_by_layout_node.get(&layout_node) else {
        return true;
    };
    effects
        .iter()
        .all(|(effect, _)| pass.animated_transform_preserves_axes(effect, layout_node))
}

/// The easing a keyframe of the effect of `animation` that declares none runs: a CSS animation's, or linear.
fn default_easing_of(animation: &AnimationCell, state: &FfiAnimationTimingState) -> Easing {
    if state.kind != FfiAnimationKind::CssAnimation {
        return Easing::default();
    }
    animation.borrow().css_animation().default_easing.easing()
}

/// Whether the effect uses only replace keyframes.
fn uses_replace_keyframes(keyframes: Option<&Arc<KeyframeSet>>) -> bool {
    keyframes.is_some_and(|keyframes| !keyframes.has_additive_keyframe())
}

/// Whether a keyframe of the effect leaves a property `is_property` names out, taking the underlying value, which is
/// the output of the effects beneath the winner where there are any. The compositor only knows the element's own style.
fn takes_the_underlying_value(keyframes: Option<&Arc<KeyframeSet>>, is_property: impl Fn(u16) -> bool) -> bool {
    let Some(keyframes) = keyframes else {
        return false;
    };
    keyframes.keyframes().iter().any(|keyframe| {
        keyframes.target_properties().iter().any(|&property| {
            is_property(property)
                && keyframes
                    .declarations_of(keyframe)
                    .iter()
                    .find(|declaration| declaration.property_id == property)
                    .is_some_and(|declaration| declaration.value == KeyframeValue::ElementValue)
        })
    })
}

/// The candidates of a pass: the associated animations of the document with associated effects, which the host keeps
/// alive through `keep_alive` with `context` for the pass.
fn candidates_of(
    animations: &mut DocumentAnimations,
    keep_alive: Option<(FfiKeepAlive, *mut c_void)>,
) -> Vec<(AnimationHandle, EffectHandle)> {
    animations
        .set_mut(DocumentAnimationSetKind::Associated)
        .alive()
        .into_iter()
        .filter_map(|animation| {
            if let Some((keep_alive, context)) = keep_alive {
                // SAFETY: The host keeps the animation alive for the pass.
                unsafe { keep_alive(context, &animation) };
            }
            let effect = animation.borrow().effect().cloned()?;
            Some((animation, effect))
        })
        .collect()
}

/// https://www.w3.org/TR/web-animations-1/#animation-composite-order
/// Updates which of the associated animations of a document whose animations are `animations` the compositor runs,
/// replaces or throttles, through `host`.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which asking for a sample reaches, and `host` must
/// hold live callbacks and a live document host for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_update_compositor_animations(
    animations: *mut DocumentAnimations,
    host: &FfiCompositorUpdateHost,
) -> FfiCompositorUpdateOutcome {
    let call = |function: unsafe extern "C" fn(*mut c_void)| {
        // SAFETY: The host answers synchronously.
        unsafe { function(host.context) }
    };
    let mut pass = Pass {
        host,
        competing_effects: Vec::new(),
        competing_index: HashMap::new(),
        in_effect_transform_effects_by_layout_node: HashMap::new(),
        animated_transform_preserves_axes: RefCell::new(HashMap::new()),
    };
    let mut candidates = Vec::new();
    let mut transform_targets = Vec::new();
    // SAFETY: Guaranteed by the caller, and the borrow ends before any sample is asked for.
    let alive_candidates = candidates_of(unsafe { &mut *animations }, Some((host.keep_alive, host.context)));
    for (animation, effect) in alive_candidates {
        let facts = EffectFacts::of(&effect);
        let was_throttled = {
            let record = effect.borrow();
            record.has_flag(EffectFlag::CompositorDriven)
                || record.has_flag(EffectFlag::CompositorReplaced)
                || record.has_flag(EffectFlag::OffscreenThrottled)
        };
        let was_published = has_retained_compositor_animations(&effect);
        if let Some(target) = facts.target
            && let Some(layout_node) = pass.layout_node(target)
        {
            // SAFETY: The host answers synchronously.
            unsafe { (host.note_target_layout_node)(host.context, layout_node) };
        }
        {
            let mut record = effect.borrow_mut();
            record.set_flag(EffectFlag::CompositorDriven, false);
            record.set_flag(EffectFlag::CompositorReplaced, false);
            record.set_flag(EffectFlag::OffscreenThrottled, false);
            record.set_flag(EffectFlag::ObservationRelevantCompositorAnimation, false);
        }

        let state = animation.borrow().timing_state(None);
        let candidate = Candidate {
            animation: animation.clone(),
            effect: effect.clone(),
            facts,
            was_throttled,
            was_published,
        };
        let facts = &candidate.facts;
        if state.play_state == AnimationPlayState::Idle
            || (!state.is_in_effect && state.phase != AnimationEffectPhase::Before)
        {
            candidates.push(candidate);
            continue;
        }
        let Some(target) = facts.target else {
            candidates.push(candidate);
            continue;
        };
        if state.is_in_effect && facts.targets_transform() {
            transform_targets.push(FfiTransformAnimationTarget {
                element: target.element,
                effect: facts.id,
            });
        }
        let index = *pass.competing_index.entry(target).or_insert_with(|| {
            pass.competing_effects.push((target, CompetingEffects::default()));
            pass.competing_effects.len() - 1
        });
        let add_competing_effect =
            |pass: &mut Pass, select: fn(&mut CompetingEffects) -> &mut CompetingPropertyEffects| {
                let winner_animation = select(&mut pass.competing_effects[index].1)
                    .winner
                    .as_ref()
                    .map(|(_, animation)| animation.clone());
                let effect_produces_output = state.is_in_effect;
                let winner_produces_output = winner_animation
                    .as_ref()
                    .is_some_and(|animation| animation.borrow().timing_state(None).is_in_effect);
                let replaces_winner = match &winner_animation {
                    None => true,
                    Some(winner_animation) => {
                        (effect_produces_output && !winner_produces_output)
                            || (effect_produces_output == winner_produces_output
                                && pass.composite_order_is_before(winner_animation, &animation))
                    }
                };
                let property_effects = select(&mut pass.competing_effects[index].1);
                property_effects.effect_count += 1;
                if effect_produces_output && !facts.composite_is_replace {
                    property_effects.all_effects_use_replace = false;
                }
                if replaces_winner {
                    property_effects.winner = Some((effect.clone(), animation.clone()));
                }
            };
        if facts.targets(property_id::OPACITY) {
            add_competing_effect(&mut pass, |effects| &mut effects.opacity);
        }
        if facts.targets(property_id::BACKGROUND_COLOR) {
            add_competing_effect(&mut pass, |effects| &mut effects.background_color);
        }
        if facts.targets(property_id::FILTER) {
            add_competing_effect(&mut pass, |effects| &mut effects.filter);
        }
        if facts.targets_transform() {
            add_competing_effect(&mut pass, |effects| &mut effects.transform);
            if let Some(layout_node) = pass.layout_node(target) {
                pass.in_effect_transform_effects_by_layout_node
                    .entry(layout_node)
                    .or_default()
                    .push((effect.clone(), animation.clone()));
            }
        }
        candidates.push(candidate);
    }

    for (_, effects) in &mut pass.competing_effects {
        let validate_winner = |property_effects: &mut CompetingPropertyEffects, is_property: fn(u16) -> bool| {
            let keyframes = property_effects
                .winner
                .as_ref()
                .and_then(|(winner, _)| winner.borrow().keyframes().cloned());
            if !property_effects.all_effects_use_replace
                || (property_effects.winner.is_some() && !uses_replace_keyframes(keyframes.as_ref()))
                || (property_effects.winner.is_some()
                    && property_effects.effect_count > 1
                    && takes_the_underlying_value(keyframes.as_ref(), is_property))
            {
                property_effects.winner = None;
            }
        };
        validate_winner(&mut effects.opacity, |property| property == property_id::OPACITY);
        validate_winner(&mut effects.background_color, |property| {
            property == property_id::BACKGROUND_COLOR
        });
        validate_winner(&mut effects.filter, |property| property == property_id::FILTER);
        validate_winner(&mut effects.transform, is_transform_family);
    }

    // NB: An opacity of 1 does not otherwise create an effects frame. Give an otherwise publishable opacity
    //     animation a target before the normal handoff loop, and retain that target while the effect remains live.
    let mut forced_opacity_effects_layer = false;
    for index in 0..pass.competing_effects.len() {
        let target = pass.competing_effects[index].0;
        let Some((effect, _)) = pass.competing_effects[index].1.opacity.winner.clone() else {
            continue;
        };
        let facts = EffectFacts::of(&effect);
        let can_force_opacity_effects_layer = !facts.has_custom_properties()
            && facts.properties().iter().all(|&property| {
                matches!(property, property_id::OPACITY | property_id::BACKGROUND_COLOR)
                    || is_transform_family(property)
            });
        // SAFETY: The host answers synchronously.
        if unsafe { (host.visibility_is_observed)(host.context, target.element) } || !can_force_opacity_effects_layer {
            continue;
        }

        let missing_visual_context_node = pass
            .build(&effect, &facts, FfiVisualAnimationTargetKind::Opacity)
            .missing_visual_context_node;
        let Some(layout_node) = pass.layout_node(target) else {
            continue;
        };
        // SAFETY: The host answers synchronously.
        unsafe {
            let already_forced_effects_layer =
                (host.frame_was_kept)(host.context, layout_node, FfiCompositorFrameKind::EffectsLayer);
            if !missing_visual_context_node && !already_forced_effects_layer {
                continue;
            }
            if missing_visual_context_node
                && (host.request_frame)(host.context, layout_node, FfiCompositorFrameKind::EffectsLayer)
            {
                forced_opacity_effects_layer = true;
            }
            (host.keep_frame)(host.context, layout_node, FfiCompositorFrameKind::EffectsLayer);
        }
    }
    if forced_opacity_effects_layer {
        call(host.refresh_viewport_mapping);
    }

    let mut outcome = FfiCompositorUpdateOutcome::default();
    let mut wakeup_delay = None;
    let mut has_throttled_animation_style_update = host.has_throttled_animation_style_update;
    let mut only_translates_horizontally_cache: HashMap<AnimationEffectId, bool> = HashMap::new();
    for candidate in &candidates {
        let effect = &candidate.effect;
        if let Some(state) = existing_state(effect) {
            // SAFETY: The effect keeps its state for as long as it lives.
            unsafe { (*state).clear_pending() };
        }
        let published_compositor_animation = update_candidate(
            &pass,
            candidate,
            &transform_targets,
            &mut wakeup_delay,
            &mut only_translates_horizontally_cache,
        );

        if let Some(state) = existing_state(effect) {
            // SAFETY: The effect keeps its state for as long as it lives.
            unsafe {
                if !published_compositor_animation {
                    (*state).clear_retained();
                }
                (*state).clear_pending();
            }
        }
        let (is_throttled, is_observation_relevant) = {
            let record = effect.borrow();
            (
                record.has_flag(EffectFlag::CompositorDriven)
                    || record.has_flag(EffectFlag::CompositorReplaced)
                    || record.has_flag(EffectFlag::OffscreenThrottled),
                record.has_flag(EffectFlag::ObservationRelevantCompositorAnimation),
            )
        };
        if is_observation_relevant {
            outcome.has_observation_relevant_compositor_animation = true;
        }
        if candidate.was_throttled && !is_throttled {
            request_observation_sample(effect);
            outcome.requested_withdrawn_effect_sample = true;
        }
        if !has_throttled_animation_style_update && can_skip_per_frame_style_update(effect) {
            call(host.note_throttled_animation_style_update);
            has_throttled_animation_style_update = true;
        }
    }
    if let Some(delay) = wakeup_delay {
        outcome.has_wakeup_delay = true;
        outcome.wakeup_delay = delay;
    }
    outcome
}

/// Decides how the compositor handles the effect of `candidate`, answering whether it published compositor animations
/// of it.
fn update_candidate(
    pass: &Pass,
    candidate: &Candidate,
    transform_targets: &[FfiTransformAnimationTarget],
    wakeup_delay: &mut Option<f64>,
    only_translates_horizontally_cache: &mut HashMap<AnimationEffectId, bool>,
) -> bool {
    let host = pass.host;
    let effect = &candidate.effect;
    let facts = &candidate.facts;
    let Some(target) = facts.target else {
        return false;
    };
    if !facts.has_target_properties() {
        return false;
    }
    let state = candidate.animation.borrow().timing_state(None);
    if state.play_state == AnimationPlayState::Idle
        || (!state.is_in_effect && state.phase != AnimationEffectPhase::Before)
    {
        return false;
    }
    let set_flag = |flag: EffectFlag, value: bool| {
        effect.borrow_mut().set_flag(flag, value);
    };
    let mut schedule_next_phase_wakeup = |state: &FfiAnimationTimingState| {
        // NB: The record decides when the effect next changes phase.
        if let Some(delay) = state.next_phase_boundary_delay.get() {
            minimum(wakeup_delay, delay);
        }
    };

    // NB: The record decides whether the timing of the effect lets it be throttled.
    if state.can_throttle_offscreen_effect && pass.paint_only_effect_is_offscreen(facts, target) {
        set_flag(EffectFlag::OffscreenThrottled, true);
        schedule_next_phase_wakeup(&state);
        return false;
    }

    let targets_opacity = facts.targets(property_id::OPACITY);
    let targets_background_color = facts.targets(property_id::BACKGROUND_COLOR);
    let targets_filter = facts.targets(property_id::FILTER);
    let targets_transform = facts.targets_transform();
    if !facts.may_run_on_the_compositor {
        return false;
    }
    let target_effects = pass
        .competing(&target)
        .expect("an effect in effect competes for its target");
    let all_targeted_properties_have_replace_winners = (!targets_opacity || target_effects.opacity.winner.is_some())
        && (!targets_background_color || target_effects.background_color.winner.is_some())
        && (!targets_filter || target_effects.filter.winner.is_some())
        && (!targets_transform || target_effects.transform.winner.is_some());
    let selected_for_opacity = targets_opacity && target_effects.opacity.is_won_by(effect);
    let selected_for_background_color = targets_background_color && target_effects.background_color.is_won_by(effect);
    let selected_for_filter = targets_filter && target_effects.filter.is_won_by(effect);
    let selected_for_transform = targets_transform && target_effects.transform.is_won_by(effect);

    // Background colors are display-list content, so associate their fill commands with a dedicated metadata
    // frame. Validate the descriptor before forcing that frame, which requires one repaint before the animation
    // can move to the compositor.
    let mut background_color_layout_node = None;
    let mut background_color_animation_was_built = false;
    if selected_for_background_color
        && let Some(layout_node) = pass.layout_node(target)
        // SAFETY: The host answers synchronously.
        && !host.force_dark_applies
        && unsafe { (host.background_color_can_be_compositor_animated)(host.context, layout_node) }
    {
        let build = pass.build(effect, facts, FfiVisualAnimationTargetKind::BackgroundColor);
        background_color_animation_was_built = build.built;
        if build.built || build.missing_visual_context_node {
            background_color_layout_node = Some(layout_node);
            if build.missing_visual_context_node {
                // SAFETY: The host answers synchronously.
                unsafe { (host.request_frame)(host.context, layout_node, FfiCompositorFrameKind::BackgroundColor) };
            }
        }
    }

    // Filters need an effects frame whose filter payload can be replaced. Validate the descriptor before forcing
    // that frame, which requires one repaint before the animation can move to the compositor.
    let mut filter_layout_node = None;
    let mut filter_animation_was_built = false;
    if selected_for_filter && let Some(layout_node) = pass.layout_node(target) {
        let build = pass.build(effect, facts, FfiVisualAnimationTargetKind::Filter);
        filter_animation_was_built = build.built;
        if build.built || build.missing_visual_context_node {
            filter_layout_node = Some(layout_node);
            if build.missing_visual_context_node {
                // SAFETY: The host answers synchronously.
                unsafe { (host.request_frame)(host.context, layout_node, FfiCompositorFrameKind::EffectsLayer) };
            }
        }
    }

    if !selected_for_opacity && !selected_for_background_color && !selected_for_filter && !selected_for_transform {
        if all_targeted_properties_have_replace_winners && state.can_throttle_replaced_effect {
            set_flag(EffectFlag::CompositorReplaced, true);
            schedule_next_phase_wakeup(&state);
        }
        return false;
    }

    let compositor_animation_state = state_of(effect);
    let mut opacity_was_handed_off = !selected_for_opacity;
    if selected_for_opacity && pass.build(effect, facts, FfiVisualAnimationTargetKind::Opacity).built {
        opacity_was_handed_off = true;
    }
    let background_color_was_handed_off = !selected_for_background_color || background_color_animation_was_built;
    // SAFETY: The host answers synchronously.
    unsafe {
        if let Some(layout_node) = background_color_layout_node {
            (host.keep_frame)(host.context, layout_node, FfiCompositorFrameKind::BackgroundColor);
        }
        if let Some(layout_node) = filter_layout_node {
            (host.keep_frame)(host.context, layout_node, FfiCompositorFrameKind::EffectsLayer);
        }
    }
    let filter_was_handed_off = !selected_for_filter || filter_animation_was_built;
    let mut transform_was_handed_off = !selected_for_transform;
    if selected_for_transform {
        let build = pass.build(effect, facts, FfiVisualAnimationTargetKind::Transform);
        transform_was_handed_off = build.built;
        if build.only_translates_horizontally_is_known {
            only_translates_horizontally_cache.insert(facts.id, build.only_translates_horizontally);
        }
    }

    // SAFETY: The host answers synchronously, and the effect keeps its state for as long as it lives.
    unsafe {
        if selected_for_opacity && (host.visibility_is_observed)(host.context, target.element) {
            (*compositor_animation_state).discard_pending(FfiVisualAnimationTargetKind::Opacity);
            opacity_was_handed_off = false;
        }
    }

    let mut transform_affects_observation = false;
    let mut requires_main_thread_observation_sampling = false;
    if selected_for_transform {
        let only_translates_horizontally = *only_translates_horizontally_cache.entry(facts.id).or_insert_with(|| {
            pass.layout_node(target).is_some_and(|layout_node| {
                pass.ask_transform_keyframes(effect, facts, layout_node, effect_only_translates_horizontally)
            })
        });
        let query = FfiTransformObservationQuery {
            effect: facts.id,
            only_translates_horizontally,
            transform_targets: transform_targets.as_ptr(),
            transform_target_count: transform_targets.len(),
            context: std::ptr::from_ref(pass).cast(),
            transform_animations_preserve_axes,
        };
        // SAFETY: The host answers synchronously.
        transform_affects_observation = unsafe {
            (host.transform_affects_intersection_observation)(
                host.context,
                target.element,
                &query,
                &mut requires_main_thread_observation_sampling,
            )
        };
    }
    // SAFETY: The effect keeps its state for as long as it lives.
    let compositor_animation_state = unsafe { &mut *compositor_animation_state };
    if requires_main_thread_observation_sampling {
        compositor_animation_state.discard_pending(FfiVisualAnimationTargetKind::Transform);
        transform_was_handed_off = false;
    }

    if !compositor_animation_state.has_pending() {
        // SAFETY: The host answers synchronously.
        if selected_for_transform
            && !transform_affects_observation
            && unsafe { (host.transform_subtree_is_clipped_outside_viewport)(host.context, target.element) }
        {
            set_flag(EffectFlag::OffscreenThrottled, true);
        }
        return false;
    }
    if all_targeted_properties_have_replace_winners
        && opacity_was_handed_off
        && background_color_was_handed_off
        && filter_was_handed_off
        && transform_was_handed_off
    {
        set_flag(EffectFlag::CompositorDriven, true);
    }
    set_flag(
        EffectFlag::ObservationRelevantCompositorAnimation,
        transform_affects_observation,
    );
    schedule_next_phase_wakeup(&state);
    // OPTIMIZATION: Ordinary rendering updates advance the WebContent timeline without changing compositor
    //               playback. An effect published before keeps the anchors of the animations whose parameters
    //               did not change.
    let published = compositor_animation_state.publish_pending(candidate.was_published);
    // SAFETY: The document host is live for the pass.
    unsafe { &*host.document_host }.publish_compositor_animations(published);
    if let Some(layout_node) = pass.layout_node(target) {
        // SAFETY: The host answers synchronously.
        unsafe { (host.note_retained_layout_node)(host.context, layout_node) };
    }
    true
}

/// The local time of the next iteration event of a CSS animation whose ticks are throttled, whose timing is `state`.
pub(crate) fn next_throttled_animation_iteration_event_time(
    state: &FfiAnimationTimingState,
    effect: &AnimationEffectCell,
) -> Option<f64> {
    // NB: The record decides this as far as the timing goes.
    let time = state.next_throttled_iteration_event_time.get()?;
    if !can_skip_per_frame_style_update(effect) || state.phase != AnimationEffectPhase::Active {
        return None;
    }
    Some(time)
}

/// Wakes the throttled and compositor-driven animations of a document whose animations are `animations` at
/// `timestamp`: asks for the samples of those that reached an instant the main thread must see, and answers when the
/// next is due.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which asking for a sample reaches.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_service_compositor_animation_wakeup(
    animations: *mut DocumentAnimations,
    timestamp: f64,
) -> FfiCompositorWakeupOutcome {
    let mut next_wakeup_delay = None;
    let mut outcome = FfiCompositorWakeupOutcome::default();
    // SAFETY: Guaranteed by the caller, and the borrow ends before any sample is asked for.
    let candidates = candidates_of(unsafe { &mut *animations }, None);
    for (animation, effect) in candidates {
        let state = animation.borrow().timing_state(Some(timestamp));
        let Some(current_time) = state.current_time_at_timestamp.get() else {
            continue;
        };
        if current_time.r#type != TimeValueType::Milliseconds {
            continue;
        }
        let current_time = current_time.value;
        let timing = state.effect;
        let playback_rate = state.playback.playback_rate;
        let (is_compositor_driven_or_replaced, is_offscreen_throttled) = {
            let record = effect.borrow();
            (
                record.has_flag(EffectFlag::CompositorDriven) || record.has_flag(EffectFlag::CompositorReplaced),
                record.has_flag(EffectFlag::OffscreenThrottled),
            )
        };
        let is_compositor_handled = is_compositor_driven_or_replaced || has_retained_compositor_animations(&effect);
        if !state.playback.has_pending_play_task
            && !state.playback.has_pending_pause_task
            && playback_rate > 0.0
            && timing.start_delay.r#type == TimeValueType::Milliseconds
            && state.phase == AnimationEffectPhase::Before
        {
            if current_time < timing.start_delay.value {
                minimum(
                    &mut next_wakeup_delay,
                    (timing.start_delay.value - current_time) / playback_rate,
                );
            } else {
                request_observation_sample(&effect);
                outcome.reached_wakeup = true;
                if is_compositor_handled {
                    outcome.reached_compositor_active_start = true;
                }
            }
        }
        if let Some(iteration_event_time) = next_throttled_animation_iteration_event_time(&state, &effect)
            && !can_skip_per_frame_animation_tick(&effect)
        {
            if current_time < iteration_event_time {
                minimum(
                    &mut next_wakeup_delay,
                    (iteration_event_time - current_time) / playback_rate,
                );
            } else {
                outcome.reached_wakeup = true;
            }
        }
        let is_offscreen_handled = is_offscreen_throttled && can_skip_per_frame_style_update(&effect);
        if (!is_compositor_handled && !is_offscreen_handled) || timing.iteration_count.is_infinite() {
            continue;
        }
        let active_end = timing.start_delay.value + timing.iteration_duration.value * timing.iteration_count;
        if current_time < active_end {
            minimum(&mut next_wakeup_delay, (active_end - current_time) / playback_rate);
            continue;
        }
        request_observation_sample(&effect);
        outcome.reached_wakeup = true;
    }
    if let Some(delay) = next_wakeup_delay {
        outcome.has_next_wakeup_delay = true;
        outcome.next_wakeup_delay = delay;
    }
    outcome
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn web_animation_set_needs_animated_style_update(
        _effect: *const std::ffi::c_void,
        _target: std::ptr::NonNull<std::ffi::c_void>,
    ) {
        unreachable!("the tests update no compositor animations")
    }
}
