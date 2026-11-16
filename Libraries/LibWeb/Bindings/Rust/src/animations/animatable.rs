/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.csswg.org/web-animations-1/#the-animatable-interface-mixin

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Object};
use libweb_rust::AnimationRecordRef;
use libweb_rust::web_animations::{
    element_node_document, sort_in_composite_order, web_animation_compare_elements_in_tree_order,
};

use super::animation::{
    AnyAnimation, TimelineRangeValue, animation_object, parse_timeline_range_edge, perform_host_effect,
    realm_of_element,
};
use super::animation_timeline::{AnyAnimationTimeline, element_of_wrapper};
use super::keyframe_effect::{
    KeyframeEffect, KeyframeEffectOptions, KeyframesArgument, UnrestrictedDoubleOrKeyframeEffectOptions,
    process_a_keyframes_argument, pseudo_element_parsing,
};
use crate::bindings::conversions::CppInterface;
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_animation_for_each_associated_animation(
        node_wrapper: *mut c_void,
        subtree: bool,
        context: *mut c_void,
        on_animation: unsafe extern "C" fn(context: *mut c_void, animation: *const c_void),
    );
}

// https://drafts.csswg.org/web-animations-1/#dictdef-keyframeanimationoptions
pub(crate) struct KeyframeAnimationOptions {
    pub(crate) parent: KeyframeEffectOptions,
    pub(crate) id: Utf16String,
    pub(crate) timeline: Option<Option<AnyAnimationTimeline>>,
    // https://drafts.csswg.org/web-animations-2/#dom-keyframeanimationoptions-rangestart
    pub(crate) range_start: TimelineRangeValue,
    // https://drafts.csswg.org/web-animations-2/#dom-keyframeanimationoptions-rangeend
    pub(crate) range_end: TimelineRangeValue,
}

/// The union (unrestricted double or KeyframeAnimationOptions) of the options of Animatable.animate().
pub(crate) enum UnrestrictedDoubleOrKeyframeAnimationOptions {
    UnrestrictedDouble(f64),
    KeyframeAnimationOptions(KeyframeAnimationOptions),
}

/// https://drafts.csswg.org/web-animations-1/#dom-animatable-animate
pub(crate) fn animate(
    this: &CppInterface,
    cx: &mut CallContext,
    keyframes: Option<Gc<Object>>,
    options: UnrestrictedDoubleOrKeyframeAnimationOptions,
) -> WebIdlResult<AnyAnimation> {
    let vm = cx.vm();

    // 1. Let target be the object on which this method was called.
    let target = element_of_wrapper(*this);

    // INTEROP: Like Chrome, the keyframes are processed before the range is parsed, so the keyframes' getters run first.
    let keyframes = process_a_keyframes_argument(vm, keyframes)?;

    // NB: The getters of the keyframes can move the target to another document, whose default document timeline the
    //     animation then gets.
    let document = element_node_document(target);
    let realm = realm_of_element(target);

    let (effect_options, animation_options) = match options {
        UnrestrictedDoubleOrKeyframeAnimationOptions::UnrestrictedDouble(duration) => (
            UnrestrictedDoubleOrKeyframeEffectOptions::UnrestrictedDouble(duration),
            None,
        ),
        UnrestrictedDoubleOrKeyframeAnimationOptions::KeyframeAnimationOptions(options) => (
            UnrestrictedDoubleOrKeyframeEffectOptions::KeyframeEffectOptions(options.parent),
            Some((options.id, options.timeline, options.range_start, options.range_end)),
        ),
    };

    // https://drafts.csswg.org/web-animations-2/#dom-keyframeanimationoptions-rangestart
    // https://drafts.csswg.org/web-animations-2/#dom-keyframeanimationoptions-rangeend
    let mut timeline_option = None;
    let mut id = None;
    let mut range = None;
    if let Some((options_id, options_timeline, range_start, range_end)) = animation_options {
        range = Some((
            parse_timeline_range_edge(document, range_start, false)?,
            parse_timeline_range_edge(document, range_end, true)?,
        ));
        timeline_option = options_timeline;
        id = Some(options_id);
    }

    // 2. Construct a new KeyframeEffect object, effect, in the relevant Realm of target by using the same procedure as
    //    the KeyframeEffect(target, keyframes, options) constructor, passing target as the target argument, and the
    //    keyframes and options arguments as supplied.
    //
    //    If the above procedure causes an exception to be thrown, propagate the exception and abort this procedure.
    let mut effect_cx = CallContext::new(vm, realm);
    let effect = KeyframeEffect::construct_for_target(
        &mut effect_cx,
        target.as_ptr(),
        KeyframesArgument::Processed(keyframes),
        effect_options,
    )?;

    // 3. If options is a KeyframeAnimationOptions object, let timeline be the timeline member of options or, if
    //    timeline member of options is missing, the default document timeline of the node document of the element on
    //    which this method was called.
    let timeline = match timeline_option {
        Some(timeline) => timeline.map(|timeline| timeline.record().clone()),
        None => Some(libweb_rust::web_animations::TimelineRecordRef::default_of_document(
            document,
        )),
    };

    // 4. Construct a new Animation object, animation, in the relevant Realm of target by using the same procedure as
    //    the Animation() constructor, passing effect and timeline as arguments of the same name.
    let animation = super::animation::Animation::create(vm, realm, Some(effect.effect().record()), timeline.as_ref());

    // 5. If options is a KeyframeAnimationOptions object, assign the value of the id member of options to animation’s
    //    id attribute.
    if let Some(id) = id {
        animation.record().set_dom_id(&id);
    }

    // 6. Run the procedure to play an animation for animation with the auto-rewind flag set to true.
    animation
        .record()
        .play_an_animation(perform_host_effect)
        .map_err(|exception| match exception {
            libweb_rust::web_animations::AnimationException::InvalidState(message) => {
                Exception::Dom("InvalidStateError", Utf16String::from_utf8(message))
            }
            libweb_rust::web_animations::AnimationException::TypeError(message) => Exception::type_error(message),
        })?;

    if let Some((start, end)) = range {
        animation.record().set_range_edge(false, start, perform_host_effect);
        animation.record().set_range_edge(true, end, perform_host_effect);
    }

    // 7. Return animation.
    Ok(AnyAnimation::Animation(animation))
}

// https://drafts.csswg.org/web-animations-1/#dictdef-getanimationsoptions
pub(crate) struct GetAnimationsOptions {
    pub(crate) subtree: bool,
    pub(crate) pseudo_element: Option<Utf16String>,
}

/// https://drafts.csswg.org/web-animations-1/#dom-animatable-getanimations
pub(crate) fn get_animations<'vm>(
    this: &CppInterface,
    cx: &mut CallContext<'vm>,
    options: GetAnimationsOptions,
) -> WebIdlResult<MarkedVec<'vm, AnyAnimation>> {
    // 1. Let object be the object on which this method was called.
    // 2. Let pseudoElement be the result of pseudo-element parsing applied to pseudoElement of options, or null if
    //    options is not passed.
    let _pseudo_element = pseudo_element_parsing(options.pseudo_element.as_ref())?;

    // 3. If pseudoElement is not null, then let target be the pseudo-element identified by pseudoElement with object
    //    as the originating element. Otherwise, let target be object.
    // FIXME: We can't refer to pseudo-elements directly, and they also can't be animated yet.

    // 4. If options is passed with subtree set to true, then return the set of relevant animations for a subtree of
    //    target. Otherwise, return the set of relevant animations for target.
    // The list returned by the above algorithm is sorted using the composite order described for the associated
    // animations of effects in § 5.4.2 The effect stack.
    Ok(sorted_relevant_animations(this, options.subtree, cx))
}

/// The relevant animations of the element or the document or shadow root `node`, and those of its subtree where
/// `subtree` is set, after the pending style changes are applied, sorted in composite order.
pub(super) fn sorted_relevant_animations<'vm>(
    node: &CppInterface,
    subtree: bool,
    cx: &mut CallContext<'vm>,
) -> MarkedVec<'vm, AnyAnimation> {
    unsafe extern "C" fn on_animation(context: *mut c_void, animation: *const c_void) {
        // SAFETY: The context is the list below, and LibWeb passes animations that it holds references to.
        let animations = unsafe { &mut *context.cast::<Vec<AnimationRecordRef>>() };
        animations.push(unsafe { AnimationRecordRef::retained_from_host(animation) });
    }
    let mut relevant_animations: Vec<AnimationRecordRef> = Vec::new();
    // SAFETY: The wrapper is live, and the callback receives the list for as long as the call runs.
    unsafe {
        web_animation_for_each_associated_animation(
            node.wrapper().as_ptr().cast(),
            subtree,
            (&raw mut relevant_animations).cast(),
            on_animation,
        );
    }
    relevant_animations.retain(AnimationRecordRef::is_exposed_relevant);

    sort_in_composite_order(&mut relevant_animations, web_animation_compare_elements_in_tree_order);

    let vm = cx.vm();
    let animations = MarkedVec::new(vm);
    for animation in &relevant_animations {
        animations.push(animation_object(vm, animation));
    }
    animations
}
