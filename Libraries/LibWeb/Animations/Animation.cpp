/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>.
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/AnimationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(Animation);

// https://www.w3.org/TR/web-animations-1/#dom-animation-animation
GC::Ref<Animation> Animation::create(JS::Realm& realm, GC::Ptr<AnimationEffect> effect, Optional<GC::Ptr<AnimationTimeline>> timeline)
{
    // 1. Let animation be a new Animation object.
    auto animation = realm.create<Animation>(realm);

    // 2. Run the procedure to set the timeline of an animation on animation passing timeline as the new timeline or, if
    //    a timeline argument is missing, passing the default document timeline of the Document associated with the
    //    Window that is the current global object.
    if (!timeline.has_value()) {
        auto& window = as<HTML::Window>(HTML::current_principal_global_object());
        timeline = window.associated_document().timeline();
    }
    animation->set_timeline(timeline.release_value());

    // 3. Run the procedure to set the associated effect of an animation on animation passing source as the new effect.
    animation->set_effect(effect);

    return animation;
}

WebIDL::ExceptionOr<GC::Ref<Animation>> Animation::construct_impl(JS::Realm& realm, GC::Ptr<AnimationEffect> effect, Optional<GC::Ptr<AnimationTimeline>> timeline)
{
    return create(realm, effect, timeline);
}

// https://www.w3.org/TR/web-animations-1/#animation-set-the-associated-effect-of-an-animation
void Animation::set_effect(GC::Ptr<AnimationEffect> new_effect)
{
    // Setting this attribute updates the object’s associated effect using the procedure to set the associated effect of
    // an animation.

    // 1. Let old effect be the current associated effect of animation, if any.
    auto old_effect = m_effect;

    // 2. If new effect is the same object as old effect, abort this procedure.
    if (new_effect == old_effect)
        return;

    // 3. If animation has a pending pause task, reschedule that task to run as soon as animation is ready.
    // 4. If animation has a pending play task, reschedule that task to run as soon as animation is ready to play ne
    //    effect.
    // Note: There is no real difference between "pending" and "as soon as possible", so this step is a no-op.

    // 5. If new effect is not null and if new effect is the associated effect of another animation, previous animation,
    //    run the procedure to set the associated effect of an animation (this procedure) on previous animation passing
    //    null as new effect.
    if (new_effect && new_effect->associated_animation() != this) {
        if (auto animation = new_effect->associated_animation())
            animation->set_effect({});
    }

    // 6. Let the associated effect of animation be new effect.
    auto old_target = m_effect ? m_effect->target() : nullptr;
    auto new_target = new_effect ? new_effect->target() : nullptr;
    if (old_target != new_target) {
        if (old_target)
            old_target->disassociate_with_animation(*this);
        if (new_target)
            new_target->associate_with_animation(*this);
    }
    if (new_effect)
        new_effect->set_associated_animation(this);
    if (m_effect)
        m_effect->set_associated_animation({});
    m_effect = new_effect;

    // Once animated properties of the old effect no longer apply, we need to ensure appropriate invalidations are scheduled
    if (old_effect) {
        AnimationUpdateContext context;
        old_effect->update_computed_properties(context);
    }

    // 7. Run the procedure to update an animation’s finished state for animation with the did seek flag set to false,
    //    and the synchronously notify flag set to false.
    update_finished_state(DidSeek::No, SynchronouslyNotify::No);
}

// https://www.w3.org/TR/web-animations-1/#animation-set-the-timeline-of-an-animation
// https://drafts.csswg.org/web-animations-2/#setting-the-timeline
void Animation::set_timeline(GC::Ptr<AnimationTimeline> new_timeline)
{
    // 1. Let old timeline be the current timeline of animation, if any.
    auto old_timeline = m_timeline;

    // 2. If new timeline is the same object as old timeline, abort this procedure.
    if (new_timeline == old_timeline)
        return;

    // 3. Let previous play state be animation’s play state.
    auto previous_play_state = play_state();

    // 4. Let previous current time be the animation’s current time.
    auto previous_current_time = current_time();

    // 5. Set previous progress based in the first condition that applies:
    auto previous_progress = [&]() -> Optional<double> {
        // If previous current time is unresolved:
        // Set previous progress to unresolved.
        if (!previous_current_time.has_value())
            return {};

        // If end time is zero:
        // Set previous progress to zero.
        if (m_effect && m_effect->end_time().value == 0)
            return 0;

        // Otherwise
        // Set previous progress = previous current time / end time
        return previous_current_time.value() / m_effect->end_time();
    }();

    // 6. Let from finite timeline be true if old timeline is not null and not monotonically increasing.
    auto from_finite_timeline = old_timeline && !old_timeline->is_monotonically_increasing();

    // 7. Let to finite timeline be true if timeline is not null and not monotonically increasing.
    auto to_finite_timeline = new_timeline && !new_timeline->is_monotonically_increasing();

    // 8. Let the timeline of animation be new timeline.
    if (m_timeline)
        m_timeline->disassociate_with_animation(*this);
    m_timeline = new_timeline;
    if (m_timeline)
        m_timeline->associate_with_animation(*this);

    auto const previous_progress_multiplied_by_end_time = [&]() -> TimeValue {
        // AD-HOC: The spec doesn't say what to do if we have no effect so we just assume an end time of 0
        if (!m_effect)
            return TimeValue::create_zero(m_timeline);

        return TimeValue {
            m_timeline && m_timeline->is_progress_based() ? TimeValue::Type::Percentage : TimeValue::Type::Milliseconds,
            m_effect->end_time().value * previous_progress.value()
        };
    };

    // AD-HOC: The normalization of the specified timing of the associated effect depends on the associated timeline.
    //         This must be done before calling set_current_time_for_bindings() to ensure consistent units
    if (m_effect)
        m_effect->normalize_specified_timing();

    // 9. Perform the steps corresponding to the first matching condition from the following, if any:
    // If to finite timeline,
    if (to_finite_timeline) {
        // 1. Apply any pending playback rate on animation
        apply_any_pending_playback_rate();

        // 2. set auto align start time to true.
        m_auto_align_start_time = true;

        // 3. Set start time to unresolved.
        m_start_time = {};

        // 4. Set hold time to unresolved.
        m_hold_time = {};

        // 5. If previous play state is "finished" or "running"
        if (first_is_one_of(previous_play_state, Bindings::AnimationPlayState::Finished, Bindings::AnimationPlayState::Running)) {
            // 1. Schedule a pending play task
            m_pending_play_task = TaskState::Scheduled;
        }

        // 6. If previous play state is "paused" and previous progress is resolved:
        if (previous_play_state == Bindings::AnimationPlayState::Paused && previous_progress.has_value()) {
            // 1. Set hold time to previous progress * end time.
            m_hold_time = previous_progress_multiplied_by_end_time();
        }
        // NOTE: This step ensures that previous progress is preserved even in the case of a pause-pending animation with a resolved start time.
    }
    // If from finite timeline and previous progress is resolved,
    else if (from_finite_timeline && previous_progress.has_value()) {
        // Run the procedure to set the current time to previous progress * end time.
        // NB: We know that the time we are passing is valid so we can wrap this in a MUST() to avoid error handling
        MUST(set_current_time_for_bindings(previous_progress_multiplied_by_end_time().as_css_numberish(realm())));
    }

    // 10. If the start time of animation is resolved, make animation’s hold time unresolved.
    if (m_start_time.has_value())
        m_hold_time = {};

    // 11. Run the procedure to update an animation’s finished state for animation with the did seek flag set to false,
    //    and the synchronously notify flag set to false.
    update_finished_state(DidSeek::No, SynchronouslyNotify::No);
}

// https://drafts.csswg.org/web-animations-2/#validating-a-css-numberish-time
WebIDL::ExceptionOr<Optional<TimeValue>> Animation::validate_a_css_numberish_time(Optional<CSS::CSSNumberish> const& time) const
{
    // The procedure to validate a CSSNumberish time for an input value of time is based on the first condition that matches:

    // If all of the following conditions are true:
    if (
        // The animation is associated with a progress-based timeline, and
        m_timeline && m_timeline->is_progress_based() &&

        // time is not a CSSNumeric value with percent units:
        (!time.has_value() || !time->has<GC::Root<CSS::CSSNumericValue>>() || !time->get<GC::Root<CSS::CSSNumericValue>>()->type().matches_percentage())) {
        // throw a TypeError.
        // return false;
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "CSSNumberish must be a percentage for progress-based animations"sv
        };
    }

    // If all of the following conditions are true:
    if (
        // The animation is not associated with a progress-based timeline, and
        (!m_timeline || !m_timeline->is_progress_based()) &&

        // time is a CSSNumericValue, and
        time.has_value() && time->has<GC::Root<CSS::CSSNumericValue>>() &&

        // the units of time are not duration units:
        !time->get<GC::Root<CSS::CSSNumericValue>>()->type().matches_time({}) &&

        // AD-HOC: While it's not mentioned in the spec WPT also expects us to support CSSNumericValue number value, see
        //         https://github.com/w3c/csswg-drafts/issues/13196
        !time->get<GC::Root<CSS::CSSNumericValue>>()->type().matches_number({})) {
        // throw a TypeError.
        // return false.
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "CSSNumericValue must be a time for non-progress based animations"sv
        };
    }

    // Otherwise
    // return true.

    // AD-HOC: The spec doesn't say when we should absolutize the validated value so we do it here and return the
    //         absolutized value instead of a boolean
    if (!time.has_value())
        return OptionalNone {};

    // FIXME: Figure out which element we should use for this, for now we just use the document element of the current
    //        window
    return TimeValue::from_css_numberish(time.value(), DOM::AbstractElement { *as<HTML::Window>(realm().global_object()).associated_document().document_element() });

    VERIFY_NOT_REACHED();
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-starttime
// https://www.w3.org/TR/web-animations-1/#set-the-start-time
WebIDL::ExceptionOr<void> Animation::set_start_time_for_bindings(Optional<CSS::CSSNumberish> const& raw_new_start_time)
{
    // Setting this attribute updates the start time using the procedure to set the start time of this object to the new
    // value.

    // 1. Let valid start time be the result of running the validate a CSSNumberish time procedure with new start time
    //    as the input.
    // 2. If valid start time is false, abort this procedure.
    // AD-HOC: The validate_a_css_numberish_time throws on validation failure which is handled by the TRY() macro so
    //         there is no need to assign the `valid start time` variable here.
    auto new_start_time = TRY(validate_a_css_numberish_time(raw_new_start_time));

    // 3. Set auto align start time to false.
    m_auto_align_start_time = false;

    // 4. Let timeline time be the current time value of the timeline that animation is associated with. If there is no
    //    timeline associated with animation or the associated timeline is inactive, let the timeline time be
    //    unresolved.
    auto timeline_time = m_timeline && !m_timeline->is_inactive() ? m_timeline->current_time() : Optional<TimeValue> {};

    // 5. If timeline time is unresolved and new start time is resolved, make animation’s hold time unresolved.
    if (!timeline_time.has_value() && new_start_time.has_value())
        m_hold_time = {};

    // 6. Let previous current time be animation’s current time.
    auto previous_current_time = current_time();

    // 7. Apply any pending playback rate on animation.
    apply_any_pending_playback_rate();

    // 8. Set animation’s start time to new start time.
    m_start_time = new_start_time;

    // 9. Update animation’s hold time based on the first matching condition from the following,

    // -> If new start time is resolved,
    if (new_start_time.has_value()) {
        // If animation’s playback rate is not zero, make animation’s hold time unresolved.
        if (m_playback_rate != 0.0)
            m_hold_time = {};
    }
    // -> Otherwise (new start time is unresolved),
    else {
        // Set animation’s hold time to previous current time even if previous current time is unresolved.
        m_hold_time = previous_current_time;
    }

    // 10. If animation has a pending play task or a pending pause task, cancel that task and resolve animation’s current
    //    ready promise with animation.
    if (pending()) {
        m_pending_play_task = TaskState::None;
        m_pending_pause_task = TaskState::None;
        WebIDL::resolve_promise(realm(), current_ready_promise(), this);
    }

    // 11. Run the procedure to update an animation’s finished state for animation with the did seek flag set to true,
    //    and the synchronously notify flag set to false.
    update_finished_state(DidSeek::Yes, SynchronouslyNotify::No);

    return {};
}

// https://drafts.csswg.org/web-animations-2/#auto-aligning-start-time
void Animation::calculate_auto_aligned_start_time()
{
    VERIFY(m_timeline && m_timeline->is_progress_based());

    // 1. If the auto-align start time flag is false, abort this procedure.
    if (!m_auto_align_start_time)
        return;

    // 2. If the timeline is inactive, abort this procedure.
    if (!m_timeline || m_timeline->is_inactive())
        return;

    // 3. If play state is idle, abort this procedure.
    if (is_idle())
        return;

    // 4. If play state is paused, and hold time is resolved, abort this procedure.
    if (play_state() == Bindings::AnimationPlayState::Paused && m_hold_time.has_value())
        return;

    // 5. FIXME: Let start offset be the resolved timeline time corresponding to the start of the animation attachment
    //           range. In the case of view timelines, it requires a calculation based on the proportion of the cover
    //           range.
    auto start_offset = TimeValue { TimeValue::Type::Percentage, 0 };

    // 6. FIXME: Let end offset be the resolved timeline time corresponding to the end of the animation attachment
    //           range. In the case of view timelines, it requires a calculation based on the proportion of the cover
    //           range.
    auto end_offset = TimeValue { TimeValue::Type::Percentage, 100 };

    // 7. Set start time to start offset if effective playback rate ≥ 0, and end offset otherwise.
    if (effective_playback_rate() >= 0.0)
        m_start_time = start_offset;
    else
        m_start_time = end_offset;

    // 8. Clear hold time.
    m_hold_time = {};
}

// https://www.w3.org/TR/web-animations-1/#animation-current-time
Optional<TimeValue> Animation::current_time() const
{
    // The current time is calculated from the first matching condition from below:

    // -> If the animation’s hold time is resolved,
    if (m_hold_time.has_value()) {
        // The current time is the animation’s hold time.
        return m_hold_time.value();
    }

    // -> If any of the following are true:
    //    - the animation has no associated timeline, or
    //    - the associated timeline is inactive, or
    //    - the animation’s start time is unresolved.
    if (!m_timeline || m_timeline->is_inactive() || !m_start_time.has_value()) {
        // The current time is an unresolved time value.
        return {};
    }

    // -> Otherwise,
    //    current time = (timeline time - start time) × playback rate
    //    Where timeline time is the current time value of the associated timeline. The playback rate value is defined
    //    in §4.4.15 Speed control.
    return (m_timeline->current_time().value() - m_start_time.value()) * playback_rate();
}

// https://www.w3.org/TR/web-animations-1/#animation-set-the-current-time
WebIDL::ExceptionOr<void> Animation::set_current_time_for_bindings(Optional<CSS::CSSNumberish> const& raw_seek_time)
{
    // AD-HOC: We validate here instead of within silently_set_current_time so we have access to the `TimeValue`
    //         value within this function.
    auto seek_time = TRY(validate_a_css_numberish_time(raw_seek_time));

    // 1. Run the steps to silently set the current time of animation to seek time.
    TRY(silently_set_current_time(seek_time));

    // 2. If animation has a pending pause task, synchronously complete the pause operation by performing the following
    //    steps:
    if (m_pending_pause_task == TaskState::Scheduled) {
        // 1. Set animation’s hold time to seek time.
        m_hold_time = seek_time;

        // 2. Apply any pending playback rate to animation.
        apply_any_pending_playback_rate();

        // 3. Make animation’s start time unresolved.
        m_start_time = {};

        // 4. Cancel the pending pause task.
        m_pending_pause_task = TaskState::None;

        // 5 Resolve animation’s current ready promise with animation.
        WebIDL::resolve_promise(realm(), current_ready_promise(), this);
    }

    // 3. Run the procedure to update an animation’s finished state for animation with the did seek flag set to true,
    //    and the synchronously notify flag set to false.
    update_finished_state(DidSeek::Yes, SynchronouslyNotify::No);

    return {};
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-playbackrate
// https://www.w3.org/TR/web-animations-1/#set-the-playback-rate
WebIDL::ExceptionOr<void> Animation::set_playback_rate(double new_playback_rate)
{
    // Setting this attribute follows the procedure to set the playback rate of this object to the new value.

    // 1. Clear any pending playback rate on animation.
    m_pending_playback_rate = {};

    // 2. Let previous time be the value of the current time of animation before changing the playback rate.
    auto previous_time = current_time();

    // 3. Let previous playback rate be the current effective playback rate of animation.
    auto previous_playback_rate = playback_rate();

    // 4. Set the playback rate to new playback rate.
    m_playback_rate = new_playback_rate;

    // 5. Perform the steps corresponding to the first matching condition from the following, if any:

    // -> If animation is associated with a monotonically increasing timeline and the previous time is resolved,
    if (m_timeline && m_timeline->is_monotonically_increasing() && previous_time.has_value()) {
        // set the current time of animation to previous time.
        TRY(set_current_time_for_bindings(previous_time->as_css_numberish(realm())));
    }
    // -> If animation is associated with a non-null timeline that is not monotonically increasing, the start time of
    //    animation is resolved, associated effect end is not infinity, and either:
    //    - the previous playback rate < 0 and the new playback rate ≥ 0, or
    //    - the previous playback rate ≥ 0 and the new playback rate < 0,
    else if (m_timeline && !m_timeline->is_monotonically_increasing() && m_start_time.has_value() && !isinf(associated_effect_end().value) && ((previous_playback_rate < 0.0 && new_playback_rate >= 0.0) || (previous_playback_rate >= 0 && new_playback_rate < 0))) {
        // Set animation’s start time to the result of evaluating associated effect end - start time for animation.
        m_start_time = associated_effect_end() - m_start_time.value();
    }

    return {};
}

// https://www.w3.org/TR/web-animations-1/#animation-play-state
Bindings::AnimationPlayState Animation::play_state_for_bindings() const
{
    if (m_owning_element.has_value())
        m_owning_element->document().update_style();

    return play_state();
}

Bindings::AnimationPlayState Animation::play_state() const
{
    // The play state of animation, animation, at a given moment is the state corresponding to the first matching
    // condition from the following:

    // -> All of the following conditions are true:
    //    - The current time of animation is unresolved, and
    //    - the start time of animation is unresolved, and
    //    - animation does not have either a pending play task or a pending pause task,
    auto current_time = this->current_time();
    if (!current_time.has_value() && !m_start_time.has_value() && !pending()) {
        // → idle
        return Bindings::AnimationPlayState::Idle;
    }

    // -> Either of the following conditions are true:
    //    - animation has a pending pause task, or
    //    - both the start time of animation is unresolved and it does not have a pending play task,
    if (m_pending_pause_task == TaskState::Scheduled || (!m_start_time.has_value() && m_pending_play_task == TaskState::None)) {
        // → paused
        return Bindings::AnimationPlayState::Paused;
    }

    // -> For animation, current time is resolved and either of the following conditions are true:
    //    - animation’s effective playback rate > 0 and current time ≥ associated effect end; or
    //    - animation’s effective playback rate < 0 and current time ≤ 0,
    auto effective_playback_rate = this->effective_playback_rate();
    if (current_time.has_value() && ((effective_playback_rate > 0.0 && current_time.value() >= associated_effect_end()) || (effective_playback_rate < 0.0 && current_time->value <= 0))) {
        // → finished
        return Bindings::AnimationPlayState::Finished;
    }

    // -> Otherwise,
    //    → running
    return Bindings::AnimationPlayState::Running;
}

// https://www.w3.org/TR/web-animations-1/#animation-relevant
bool Animation::is_relevant() const
{
    // An animation is relevant if:
    // - Its associated effect is current or in effect, and
    // - Its replace state is not removed.
    return (m_effect && (m_effect->is_current() || m_effect->is_in_effect())) && replace_state() != Bindings::AnimationReplaceState::Removed;
}

// https://www.w3.org/TR/web-animations-1/#replaceable-animation
bool Animation::is_replaceable() const
{
    // An animation is replaceable if all of the following conditions are true:

    // - The existence of the animation is not prescribed by markup. That is, it is not a CSS animation with an owning
    //   element, nor a CSS transition with an owning element.
    if ((is_css_animation() || is_css_transition()) && owning_element().has_value())
        return false;

    // - The animation's play state is finished.
    if (play_state() != Bindings::AnimationPlayState::Finished)
        return false;

    // - The animation's replace state is not removed.
    if (replace_state() == Bindings::AnimationReplaceState::Removed)
        return false;

    // - The animation is associated with a monotonically increasing timeline.
    if (!m_timeline || !m_timeline->is_monotonically_increasing())
        return false;

    // - The animation has an associated effect.
    if (!m_effect)
        return false;

    // - The animation's associated effect is in effect.
    if (!m_effect->is_in_effect())
        return false;

    // - The animation's associated effect has an effect target.
    if (!m_effect->target())
        return false;

    return true;
}

void Animation::set_replace_state(Bindings::AnimationReplaceState value)
{
    if (value == Bindings::AnimationReplaceState::Removed) {
        // Remove the associated effect from its target, if applicable
        if (m_effect && m_effect->target())
            m_effect->target()->disassociate_with_animation(*this);

        // Remove this animation from its timeline
        m_timeline->disassociate_with_animation(*this);
    } else if (value == Bindings::AnimationReplaceState::Persisted && m_replace_state == Bindings::AnimationReplaceState::Removed) {
        // This animation was removed, but is now being "unremoved"; undo the effects from the if-statement above
        if (m_effect && m_effect->target())
            m_effect->target()->associate_with_animation(*this);
        m_timeline->associate_with_animation(*this);
    }

    m_replace_state = value;
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-onfinish
GC::Ptr<WebIDL::CallbackType> Animation::onfinish()
{
    return event_handler_attribute(HTML::EventNames::finish);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-onfinish
void Animation::set_onfinish(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::finish, event_handler);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-oncancel
GC::Ptr<WebIDL::CallbackType> Animation::oncancel()
{
    return event_handler_attribute(HTML::EventNames::cancel);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-oncancel
void Animation::set_oncancel(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::cancel, event_handler);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-onremove
GC::Ptr<WebIDL::CallbackType> Animation::onremove()
{
    return event_handler_attribute(HTML::EventNames::remove);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-onremove
void Animation::set_onremove(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::remove, event_handler);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-cancel
void Animation::cancel(ShouldInvalidate should_invalidate)
{
    // Note: When called from JS, we always want to invalidate the animation target's style. However, this method is
    //       also called from the StyleComputer when the animation-name CSS property changes. That happens in the
    //       middle of a cascade, and importantly, _before_ computing the animation effect stack, so there is no
    //       need for another invalidation. And in fact, if we did invalidate, it would lead to a crash, as the element
    //       would not have it's "m_needs_style_update" flag cleared.

    auto& realm = this->realm();

    // 1. If animation’s play state is not idle, perform the following steps:
    if (play_state() != Bindings::AnimationPlayState::Idle) {
        HTML::TemporaryExecutionContext execution_context { realm };

        // 1. Run the procedure to reset an animation’s pending tasks on animation.
        reset_an_animations_pending_tasks();

        // 2. Reject the current finished promise with a DOMException named "AbortError".
        auto dom_exception = WebIDL::AbortError::create(realm, "Animation was cancelled"_utf16);
        WebIDL::reject_promise(realm, current_finished_promise(), dom_exception);

        // 3. Set the [[PromiseIsHandled]] internal slot of the current finished promise to true.
        WebIDL::mark_promise_as_handled(current_finished_promise());

        // 4. Let current finished promise be a new promise in the relevant Realm of animation.
        m_current_finished_promise = WebIDL::create_promise(realm);
        m_is_finished = false;

        // 5. Create an AnimationPlaybackEvent, cancelEvent.
        // 6. Set cancelEvent’s type attribute to cancel.
        // 7. Set cancelEvent’s currentTime to null.
        // 8. Let timeline time be the current time of the timeline with which animation is associated. If animation is
        //    not associated with an active timeline, let timeline time be an unresolved time value.
        // 9. Set cancelEvent’s timelineTime to timeline time. If timeline time is unresolved, set it to null.
        AnimationPlaybackEventInit init;
        init.timeline_time = m_timeline && !m_timeline->is_inactive() ? m_timeline->current_time().map([&](auto const& value) { return value.as_css_numberish(realm); }) : Optional<CSS::CSSNumberish> {};
        auto cancel_event = AnimationPlaybackEvent::create(realm, HTML::EventNames::cancel, init);

        // 10. If animation has a document for timing, then append cancelEvent to its document for timing's pending
        //     animation event queue along with its target, animation. If animation is associated with an active
        //     timeline that defines a procedure to convert timeline times to origin-relative time, let the scheduled
        //     event time be the result of applying that procedure to timeline time. Otherwise, the scheduled event time
        //     is an unresolved time value.
        //     Otherwise, queue a task to dispatch cancelEvent at animation. The task source for this task is the DOM
        //     manipulation task source.
        if (auto document = document_for_timing()) {
            Optional<double> scheduled_event_time;
            if (m_timeline && !m_timeline->is_inactive() && m_timeline->can_convert_a_timeline_time_to_an_origin_relative_time())
                scheduled_event_time = m_timeline->convert_a_timeline_time_to_an_origin_relative_time(m_timeline->current_time());
            document->append_pending_animation_event({ cancel_event, *this, *this, scheduled_event_time });
        } else {
            HTML::queue_global_task(HTML::Task::Source::DOMManipulation, realm.global_object(), GC::create_function(heap(), [this, cancel_event]() {
                dispatch_event(cancel_event);
            }));
        }
    }

    // 2. Make animation’s hold time unresolved.
    m_hold_time = {};

    // 3. Make animation’s start time unresolved.
    m_start_time = {};

    // This time is needed for dispatching the animationcancel DOM event
    if (auto effect = m_effect)
        m_saved_cancel_time = effect->active_time_using_fill(Bindings::FillMode::Both);

    if (should_invalidate == ShouldInvalidate::Yes)
        invalidate_effect();
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-finish
WebIDL::ExceptionOr<void> Animation::finish()
{
    // 1. If animation’s effective playback rate is zero, or if animation’s effective playback rate > 0 and associated
    //    effect end is infinity, throw an "InvalidStateError" DOMException and abort these steps.
    auto effective_playback_rate = this->effective_playback_rate();
    if (effective_playback_rate == 0.0)
        return WebIDL::InvalidStateError::create(realm(), "Animation with a playback rate of 0 cannot be finished"_utf16);
    if (effective_playback_rate > 0.0 && isinf(associated_effect_end().value))
        return WebIDL::InvalidStateError::create(realm(), "Animation with no end cannot be finished"_utf16);

    // 2. Apply any pending playback rate to animation.
    apply_any_pending_playback_rate();

    // 3. Set limit as follows:
    //    -> If playback rate > 0,
    //       Let limit be associated effect end.
    //    -> Otherwise,
    //       Let limit be zero.
    auto playback_rate = this->playback_rate();
    auto limit = playback_rate > 0.0 ? associated_effect_end() : TimeValue::create_zero(m_timeline);

    // 4. Silently set the current time to limit.
    TRY(silently_set_current_time(limit));

    // 5. If animation’s start time is unresolved and animation has an associated active timeline, let the start time be
    //    the result of evaluating timeline time - (limit / playback rate) where timeline time is the current time value
    //    of the associated timeline.
    if (!m_start_time.has_value() && m_timeline && !m_timeline->is_inactive())
        m_start_time = m_timeline->current_time().value() - (limit / playback_rate);

    // 6. If there is a pending pause task and start time is resolved,
    auto should_resolve_ready_promise = false;
    if (m_pending_pause_task == TaskState::Scheduled && m_start_time.has_value()) {
        // 1. Let the hold time be unresolved.
        // Note: Typically the hold time will already be unresolved except in the case when the animation was previously
        //       idle.
        m_hold_time = {};

        // 2. Cancel the pending pause task.
        m_pending_pause_task = TaskState::None;

        // 3. Resolve the current ready promise of animation with animation.
        should_resolve_ready_promise = true;
    }

    // 7. If there is a pending play task and start time is resolved, cancel that task and resolve the current ready
    //    promise of animation with animation.
    if (m_pending_play_task == TaskState::Scheduled && m_start_time.has_value()) {
        m_pending_play_task = TaskState::None;
        should_resolve_ready_promise = true;
    }

    if (should_resolve_ready_promise) {
        HTML::TemporaryExecutionContext execution_context { realm() };
        WebIDL::resolve_promise(realm(), current_ready_promise(), this);
    }

    // 8. Run the procedure to update an animation’s finished state for animation with the did seek flag set to true,
    //    and the synchronously notify flag set to true.
    update_finished_state(DidSeek::Yes, SynchronouslyNotify::Yes);

    return {};
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-play
WebIDL::ExceptionOr<void> Animation::play()
{
    // Begins or resumes playback of the animation by running the procedure to play an animation passing true as the
    // value of the auto-rewind flag.
    return play_an_animation(AutoRewind::Yes);
}

// https://drafts.csswg.org/web-animations-1/#playing-an-animation-section
// https://drafts.csswg.org/web-animations-2/#play-an-animation
WebIDL::ExceptionOr<void> Animation::play_an_animation(AutoRewind auto_rewind)
{
    // 1. Let aborted pause be a boolean flag that is true if animation has a pending pause task, and false otherwise.
    auto aborted_pause = m_pending_pause_task == TaskState::Scheduled;

    // 2. Let has pending ready promise be a boolean flag that is initially false.
    auto has_pending_ready_promise = false;

    // 3. Let has finite timeline be true if animation has an associated timeline that is not monotonically increasing.
    auto has_finite_timeline = m_timeline && !m_timeline->is_monotonically_increasing();

    // 4. Let previous current time be the animation’s current time
    auto previous_current_time = current_time();

    // 5. Let enable seek be true if the auto-rewind flag is true and has finite timeline is false. Otherwise,
    //    initialize to false.
    auto enable_seek = (auto_rewind == AutoRewind::Yes) && !has_finite_timeline;

    // 6. Perform the steps corresponding to the first matching condition from the following, if any:
    auto const& effective_playback_rate = this->effective_playback_rate();
    auto const& associated_effect_end = this->associated_effect_end();

    // -> If animation’s effective playback rate > 0, enable seek is true and either animation’s:
    //    - previous current time is unresolved, or
    //    - previous current time < zero, or
    //    - previous current time ≥ associated effect end,
    if (effective_playback_rate > 0.0 && enable_seek && (!previous_current_time.has_value() || previous_current_time->value < 0 || previous_current_time.value() >= associated_effect_end)) {
        // Set the animation’s hold time to zero.
        m_hold_time = TimeValue::create_zero(m_timeline);
    }

    // -> If animation’s effective playback rate < 0, enable seek is true and either animation’s:
    //    - previous current time is unresolved, or
    //    - previous current time ≤ zero, or
    //    - previous current time > associated effect end,
    else if (effective_playback_rate < 0 && enable_seek && (!previous_current_time.has_value() || previous_current_time->value <= 0 || previous_current_time.value() > associated_effect_end)) {
        // -> If associated effect end is positive infinity,
        if (isinf(associated_effect_end.value) && associated_effect_end.value > 0) {
            // throw an "InvalidStateError" DOMException and abort these steps.
            return WebIDL::InvalidStateError::create(realm(), "Cannot rewind an animation with an infinite effect end"_utf16);
        }

        // -> Otherwise,
        //    Set the animation’s hold time to the animation’s associated effect end.
        m_hold_time = associated_effect_end;
    }

    // -> If animation’s effective playback rate = 0 and animation’s current time is unresolved,
    else if (effective_playback_rate == 0.0 && !previous_current_time.has_value()) {
        // Set the animation’s hold time to zero.
        m_hold_time = TimeValue::create_zero(m_timeline);
    }

    // 7. If has finite timeline and previous current time is unresolved:
    if (has_finite_timeline && !previous_current_time.has_value()) {
        // Set the flag auto align start time to true.
        m_auto_align_start_time = true;
    }

    // 8. If animation’s hold time is resolved, let its start time be unresolved.
    if (m_hold_time.has_value())
        m_start_time = {};

    // 9. If animation has a pending play task or a pending pause task,
    if (pending()) {
        // 1. Cancel that task.
        m_pending_play_task = TaskState::None;
        m_pending_pause_task = TaskState::None;

        // 2. Set has pending ready promise to true.
        has_pending_ready_promise = true;
    }

    // 10. If the following three conditions are all satisfied:
    //     - animation’s hold time is unresolved, and
    //     - aborted pause is false, and
    //     - animation does not have a pending playback rate,
    // AD-HOC: We also don't abort if we have a pending auto-alignment of the start time, see
    //         https://github.com/w3c/csswg-drafts/issues/13236
    auto pending_auto_aligned_start_time = m_auto_align_start_time && !m_start_time.has_value();
    if (!m_hold_time.has_value() && !aborted_pause && !m_pending_playback_rate.has_value() && !pending_auto_aligned_start_time) {
        // abort this procedure.
        return {};
    }

    // 11. If has pending ready promise is false, let animation’s current ready promise be a new promise in the relevant
    //     Realm of animation.
    if (!has_pending_ready_promise)
        m_current_ready_promise = WebIDL::create_promise(realm());

    // 12. Schedule a task to run as soon as animation is ready. The task shall perform the following steps:
    //
    //         Note: Steps omitted, see run_pending_play_task()
    //
    //     So long as the above task is scheduled but has yet to run, animation is described as having a pending play
    //     task. While the task is running, however, animation does not have a pending play task.
    //
    //     If a user agent determines that animation is immediately ready, it may schedule the above task as a microtask
    //     such that it runs at the next microtask checkpoint, but it must not perform the task synchronously.
    m_pending_play_task = TaskState::Scheduled;

    // 13. Run the procedure to update an animation’s finished state for animation with the did seek flag set to false,
    //     and the synchronously notify flag set to false.
    update_finished_state(DidSeek::No, SynchronouslyNotify::No);

    return {};
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-pause
WebIDL::ExceptionOr<void> Animation::pause()
{
    // 1. If animation has a pending pause task, abort these steps.
    if (m_pending_pause_task == TaskState::Scheduled)
        return {};

    // 2. If the play state of animation is paused, abort these steps.
    if (play_state() == Bindings::AnimationPlayState::Paused)
        return {};

    // 3. Let seek time be a time value that is initially unresolved.
    Optional<TimeValue> seek_time;

    // 4. Let has finite timeline be true if animation has an associated timeline that is not monotonically increasing.
    auto has_finite_timeline = m_timeline && !m_timeline->is_monotonically_increasing();

    // 5. If the animation’s current time is unresolved, perform the steps according to the first matching condition
    //    from below:
    if (!current_time().has_value()) {
        // -> If animation’s playback rate is ≥ 0,
        if (playback_rate() >= 0.0) {
            // Set seek time to zero.
            seek_time = TimeValue::create_zero(m_timeline);
        }
        // -> Otherwise
        else {
            // If associated effect end for animation is positive infinity,
            auto associated_effect_end = this->associated_effect_end();
            if (isinf(associated_effect_end.value) && associated_effect_end.value > 0) {
                // throw an "InvalidStateError" DOMException and abort these steps.
                return WebIDL::InvalidStateError::create(realm(), "Cannot pause an animation with an infinite effect end"_utf16);
            }

            // Otherwise,
            //     Set seek time to animation’s associated effect end.
            seek_time = associated_effect_end;
        }
    }

    // 6. If seek time is resolved,
    if (seek_time.has_value()) {
        // If has finite timeline is true,
        if (has_finite_timeline) {
            // Set animation’s start time to seek time.
            m_start_time = seek_time;
        }
        // Otherwise,
        else {
            // Set animation’s hold time to seek time.
            m_hold_time = seek_time;
        }
    }

    // 7. Let has pending ready promise be a boolean flag that is initially false.
    auto has_pending_ready_promise = false;

    // 8. If animation has a pending play task, cancel that task and let has pending ready promise be true.
    if (m_pending_play_task == TaskState::Scheduled) {
        m_pending_play_task = TaskState::None;
        has_pending_ready_promise = true;
    }

    // 9. If has pending ready promise is false, set animation’s current ready promise to a new promise in the relevant
    //    Realm of animation.
    if (!has_pending_ready_promise)
        m_current_ready_promise = WebIDL::create_promise(realm());

    // 10. Schedule a task to be executed at the first possible moment where both of the following conditions are true:
    // NB: Criteria has been listed out in is_ready_to_run_pending_pause_task()
    // NB: This is run_pending_pause_task()
    m_pending_pause_task = TaskState::Scheduled;

    // 11. Run the procedure to update an animation’s finished state for animation with the did seek flag set to false,
    //     and the synchronously notify flag set to false.
    update_finished_state(DidSeek::No, SynchronouslyNotify::No);

    return {};
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-updateplaybackrate
WebIDL::ExceptionOr<void> Animation::update_playback_rate(double new_playback_rate)
{
    // 1. Let previous play state be animation’s play state.
    // Note: It is necessary to record the play state before updating animation’s effective playback rate since, in the
    //       following logic, we want to immediately apply the pending playback rate of animation if it is currently
    //       finished regardless of whether or not it will still be finished after we apply the pending playback rate.
    auto previous_play_state = play_state();

    // 2. Let animation’s pending playback rate be new playback rate.
    m_pending_playback_rate = new_playback_rate;

    // 3. Perform the steps corresponding to the first matching condition from below:

    // -> If animation has a pending play task or a pending pause task,
    if (pending()) {
        // Abort these steps.
        // Note: The different types of pending tasks will apply the pending playback rate when they run so there is no
        //       further action required in this case.
        return {};
    }

    // -> If previous play state is idle or paused, or animation’s current time is unresolved,
    if (previous_play_state == Bindings::AnimationPlayState::Idle || previous_play_state == Bindings::AnimationPlayState::Paused || !current_time().has_value()) {
        // Apply any pending playback rate on animation.
        // Note: the second condition above is required so that if we have a running animation with an unresolved
        //       current time and no pending play task, we do not attempt to play it below.
        apply_any_pending_playback_rate();
    }
    // -> If previous play state is finished,
    else if (previous_play_state == Bindings::AnimationPlayState::Finished) {
        // 1. Let the unconstrained current time be the result of calculating the current time of animation
        //    substituting an unresolved time value for the hold time.
        Optional<TimeValue> unconstrained_current_time;
        {
            TemporaryChange change(m_hold_time, {});
            unconstrained_current_time = current_time();
        }

        // 2. Let animation’s start time be the result of evaluating the following expression:
        //        timeline time - (unconstrained current time / pending playback rate)
        //    Where timeline time is the current time value of the timeline associated with animation.
        //    If pending playback rate is zero, let animation’s start time be timeline time.
        if (m_pending_playback_rate.value() == 0.0) {
            m_start_time = m_timeline->current_time().value();
        } else {
            m_start_time = m_timeline->current_time().value() - (unconstrained_current_time.value() / m_pending_playback_rate.value());
        }

        // 3. Apply any pending playback rate on animation.
        apply_any_pending_playback_rate();

        // 4. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //    false, and the synchronously notify flag set to false.
        update_finished_state(DidSeek::No, SynchronouslyNotify::No);
    }
    // -> Otherwise,
    else {
        // Run the procedure to play an animation for animation with the auto-rewind flag set to false.
        TRY(play_an_animation(AutoRewind::No));
    }

    return {};
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-reverse
WebIDL::ExceptionOr<void> Animation::reverse()
{
    auto& realm = this->realm();

    // 1. If there is no timeline associated with animation, or the associated timeline is inactive throw an
    //    "InvalidStateError" DOMException and abort these steps.
    if (!m_timeline || m_timeline->is_inactive())
        return WebIDL::InvalidStateError::create(realm, "Cannot reverse an animation with an inactive timeline"_utf16);

    // 2. Let original pending playback rate be animation’s pending playback rate.
    auto original_pending_playback_rate = m_pending_playback_rate;

    // 3. Let animation’s pending playback rate be the additive inverse of its effective playback rate (i.e.
    //    -effective playback rate).
    m_pending_playback_rate = -effective_playback_rate();

    // 4. Run the steps to play an animation for animation with the auto-rewind flag set to true.
    //    If the steps to play an animation throw an exception, set animation’s pending playback rate to original
    //    pending playback rate and propagate the exception.
    auto result = play_an_animation(AutoRewind::Yes);
    if (result.is_error()) {
        m_pending_playback_rate = original_pending_playback_rate;
        return result;
    }

    return {};
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-persist
void Animation::persist()
{
    // Sets this animation’s replace state to persisted.
    set_replace_state(Bindings::AnimationReplaceState::Persisted);
}

// https://www.w3.org/TR/web-animations-1/#animation-time-to-timeline-time
Optional<TimeValue> Animation::convert_an_animation_time_to_timeline_time(Optional<TimeValue> time) const
{
    // 1. If time is unresolved, return time.
    if (!time.has_value())
        return time;

    // 2. If time is infinity, return an unresolved time value.
    if (isinf(time->value))
        return {};

    // 3. If animation’s playback rate is zero, return an unresolved time value.
    if (m_playback_rate == 0.0)
        return {};

    // 4. If animation’s start time is unresolved, return an unresolved time value.
    if (!m_start_time.has_value())
        return {};

    // 5. Return the result of calculating: time × (1 / playback rate) + start time (where playback rate and start time
    //    are the playback rate and start time of animation, respectively).
    return (time.value() * (1.0 / m_playback_rate)) + m_start_time.value();
}

// https://www.w3.org/TR/web-animations-1/#animation-time-to-origin-relative-time
Optional<double> Animation::convert_a_timeline_time_to_an_origin_relative_time(Optional<TimeValue> time) const
{
    // 1. Let timeline time be the result of converting time from an animation time to a timeline time.
    auto timeline_time = convert_an_animation_time_to_timeline_time(time);

    // 2. If timeline time is unresolved, return time.
    if (!timeline_time.has_value())
        return {};

    // 3. If animation is not associated with a timeline, return an unresolved time value.
    if (!m_timeline)
        return {};

    // 4. If animation is associated with an inactive timeline, return an unresolved time value.
    if (m_timeline->is_inactive())
        return {};

    // 5. If there is no procedure to convert a timeline time to an origin-relative time for the timeline associated
    //    with animation, return an unresolved time value.
    if (!m_timeline->can_convert_a_timeline_time_to_an_origin_relative_time())
        return {};

    // 6. Return the result of converting timeline time to an origin-relative time using the procedure defined for the
    //    timeline associated with animation.
    return m_timeline->convert_a_timeline_time_to_an_origin_relative_time(timeline_time);
}

// https://www.w3.org/TR/web-animations-1/#animation-document-for-timing
GC::Ptr<DOM::Document> Animation::document_for_timing() const
{
    // An animation’s document for timing is the Document with which its timeline is associated. If an animation is not
    // associated with a timeline, or its timeline is not associated with a document, then it has no document for
    // timing.
    if (!m_timeline)
        return {};
    return m_timeline->associated_document();
}

void Animation::update()
{
    // https://drafts.csswg.org/scroll-animations-1/#event-loop
    // When updating timeline current time, the start time of any attached animation is conditionally updated. For each
    // attached animation, run the procedure for calculating an auto-aligned start time.
    if (m_timeline && m_timeline->is_progress_based())
        calculate_auto_aligned_start_time();

    // Prevent unnecessary work if the animation is already finished and can't exit the finished state due to timeline
    // changes
    if (!m_is_finished || !m_timeline->is_monotonically_increasing())
        update_finished_state(DidSeek::No, SynchronouslyNotify::Yes);

    // Act on the pending play or pause task
    if (m_pending_play_task == TaskState::Scheduled && is_ready()) {
        m_pending_play_task = TaskState::None;
        run_pending_play_task();
    }

    if (m_pending_pause_task == TaskState::Scheduled && is_ready_to_run_pending_pause_task()) {
        m_pending_pause_task = TaskState::None;
        run_pending_pause_task();
    }
}

void Animation::effect_timing_changed(Badge<AnimationEffect>)
{
    update_finished_state(DidSeek::No, SynchronouslyNotify::Yes);
}

// https://www.w3.org/TR/web-animations-1/#associated-effect-end
TimeValue Animation::associated_effect_end() const
{
    // The associated effect end of an animation is equal to the end time of the animation’s associated effect. If the
    // animation has no associated effect, the associated effect end is zero.
    return m_effect ? m_effect->end_time() : TimeValue::create_zero(m_timeline);
}

// https://www.w3.org/TR/web-animations-1/#effective-playback-rate
double Animation::effective_playback_rate() const
{
    // The effective playback rate of an animation is its pending playback rate, if set, otherwise it is the animation’s
    // playback rate.
    return m_pending_playback_rate.has_value() ? m_pending_playback_rate.value() : m_playback_rate;
}

// https://www.w3.org/TR/web-animations-1/#apply-any-pending-playback-rate
void Animation::apply_any_pending_playback_rate()
{
    // 1. If animation does not have a pending playback rate, abort these steps.
    if (!m_pending_playback_rate.has_value())
        return;

    // 2. Set animation’s playback rate to its pending playback rate.
    m_playback_rate = m_pending_playback_rate.value();

    // 3. Clear animation’s pending playback rate.
    m_pending_playback_rate = {};
}

// https://www.w3.org/TR/web-animations-1/#animation-silently-set-the-current-time
WebIDL::ExceptionOr<void> Animation::silently_set_current_time(Optional<TimeValue> valid_seek_time)
{
    // 1. If seek time is an unresolved time value, then perform the following steps.
    if (!valid_seek_time.has_value()) {
        // 1. If the current time is resolved, then throw a TypeError.
        if (current_time().has_value()) {
            return WebIDL::SimpleException {
                WebIDL::SimpleExceptionType::TypeError,
                "Cannot change an animation's current time from a resolve value to an unresolved value"sv
            };
        }

        // 2. Abort these steps.
        return {};
    }

    // 2. Let valid seek time be the result of running the validate a CSSNumberish time procedure with seek time as the input.
    // 3. If valid seek time is false, abort this procedure.
    // AD-HOC: We have already validated in the caller.

    // 4. Set auto align start time to false.
    m_auto_align_start_time = false;

    // 5. Update either animation’s hold time or start time as follows:

    // -> If any of the following conditions are true:
    //    - animation’s hold time is resolved, or
    //    - animation’s start time is unresolved, or
    //    - animation has no associated timeline or the associated timeline is inactive, or
    //    - animation’s playback rate is 0,
    if (m_hold_time.has_value() || !m_start_time.has_value() || !m_timeline || m_timeline->is_inactive() || m_playback_rate == 0.0) {
        // Set animation’s hold time to seek time.
        m_hold_time = valid_seek_time;
    }
    // -> Otherwise,
    else {
        // Set animation’s start time to the result of evaluating timeline time - (seek time / playback rate) where
        // timeline time is the current time value of timeline associated with animation.
        m_start_time = m_timeline->current_time().value() - (valid_seek_time.value() / m_playback_rate);
    }

    // 6. If animation has no associated timeline or the associated timeline is inactive, make animation’s start time
    //    unresolved.
    if (!m_timeline || m_timeline->is_inactive())
        m_start_time = {};

    // 7. Make animation’s previous current time unresolved.
    m_previous_current_time = {};

    return {};
}

// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
void Animation::update_finished_state(DidSeek did_seek, SynchronouslyNotify synchronously_notify)
{
    auto& realm = this->realm();

    // 1. Let the unconstrained current time be the result of calculating the current time substituting an unresolved
    //    time value for the hold time if did seek is false. If did seek is true, the unconstrained current time is
    //    equal to the current time.
    //
    // Note: This is required to accommodate timelines that may change direction. Without this definition, a once-
    //       finished animation would remain finished even when its timeline progresses in the opposite direction.
    Optional<TimeValue> unconstrained_current_time;
    if (did_seek == DidSeek::No) {
        TemporaryChange change(m_hold_time, {});
        unconstrained_current_time = current_time();
    } else {
        unconstrained_current_time = current_time();
    }

    // 2. If all three of the following conditions are true,
    //    - the unconstrained current time is resolved, and
    //    - animation’s start time is resolved, and
    //    - animation does not have a pending play task or a pending pause task,
    if (unconstrained_current_time.has_value() && m_start_time.has_value() && !pending()) {
        // then update animation’s hold time based on the first matching condition for animation from below, if any:

        // -> If playback rate > 0 and unconstrained current time is greater than or equal to associated effect end,
        auto associated_effect_end = this->associated_effect_end();
        if (m_playback_rate > 0.0 && unconstrained_current_time.value() >= associated_effect_end) {
            // If did seek is true, let the hold time be the value of unconstrained current time.
            if (did_seek == DidSeek::Yes) {
                m_hold_time = unconstrained_current_time;
            }
            // If did seek is false, let the hold time be the maximum value of previous current time and associated
            // effect end. If the previous current time is unresolved, let the hold time be associated effect end.
            else if (m_previous_current_time.has_value()) {
                m_hold_time = max(m_previous_current_time.value(), associated_effect_end);
            } else {
                m_hold_time = associated_effect_end;
            }
        }
        // -> If playback rate < 0 and unconstrained current time is less than or equal to 0,
        else if (m_playback_rate < 0.0 && unconstrained_current_time->value <= 0) {
            // If did seek is true, let the hold time be the value of unconstrained current time.
            if (did_seek == DidSeek::Yes) {
                m_hold_time = unconstrained_current_time;
            }
            // If did seek is false, let the hold time be the minimum value of previous current time and zero. If the
            // previous current time is unresolved, let the hold time be zero.
            else if (m_previous_current_time.has_value()) {
                m_hold_time = min(m_previous_current_time.value(), TimeValue::create_zero(m_timeline));
            } else {
                m_hold_time = TimeValue::create_zero(m_timeline);
            }
        }
        // -> If playback rate ≠ 0, and animation is associated with an active timeline,
        else if (m_playback_rate != 0.0 && m_timeline && !m_timeline->is_inactive()) {
            // Perform the following steps:

            // 1. If did seek is true and the hold time is resolved, let animation’s start time be equal to the result
            //    of evaluating timeline time - (hold time / playback rate) where timeline time is the current time
            //    value of timeline associated with animation.
            if (did_seek == DidSeek::Yes && m_hold_time.has_value())
                m_start_time = m_timeline->current_time().value() - (m_hold_time.value() / m_playback_rate);

            // 2. Let the hold time be unresolved.
            m_hold_time = {};
        }
    }

    // 3. Set the previous current time of animation be the result of calculating its current time.
    m_previous_current_time = current_time();

    // 4. Let current finished state be true if the play state of animation is finished. Otherwise, let it be false.
    auto current_finished_state = play_state() == Bindings::AnimationPlayState::Finished;

    // 5. If current finished state is true and the current finished promise is not yet resolved, perform the following
    //    steps:
    if (current_finished_state && !m_is_finished) {
        // 1. Let finish notification steps refer to the following procedure:
        auto finish_notification_steps = GC::create_function(heap(), [this, &realm]() {
            // 1. If animation’s play state is not equal to finished, abort these steps.
            if (play_state() != Bindings::AnimationPlayState::Finished)
                return;

            // 2. Resolve animation’s current finished promise object with animation.
            WebIDL::resolve_promise(realm, current_finished_promise(), this);
            m_is_finished = true;

            // 3. Create an AnimationPlaybackEvent, finishEvent.
            // 4. Set finishEvent’s type attribute to finish.
            // 5. Set finishEvent’s currentTime attribute to the current time of animation.
            // 6. Set finishEvent’s timelineTime attribute to the current time of the timeline with which animation is
            //    associated. If animation is not associated with a timeline, or the timeline is inactive, let
            //    timelineTime be null.
            AnimationPlaybackEventInit init;
            init.current_time = current_time()->as_css_numberish(realm);
            if (m_timeline && !m_timeline->is_inactive())
                init.timeline_time = m_timeline->current_time().map([&](auto const& value) { return value.as_css_numberish(realm); });

            auto finish_event = AnimationPlaybackEvent::create(realm, HTML::EventNames::finish, init);

            // 7. If animation has a document for timing, then append finishEvent to its document for timing's pending
            //    animation event queue along with its target, animation. For the scheduled event time, use the result
            //    of converting animation’s associated effect end to an origin-relative time.
            if (auto document_for_timing = this->document_for_timing()) {
                document_for_timing->append_pending_animation_event({
                    .event = finish_event,
                    .animation = *this,
                    .target = *this,
                    .scheduled_event_time = convert_a_timeline_time_to_an_origin_relative_time(associated_effect_end()),
                });
            }
            //    Otherwise, queue a task to dispatch finishEvent at animation. The task source for this task is the DOM
            //    manipulation task source.
            else {
                // Manually create a task so its ID can be saved
                auto& document = as<HTML::Window>(realm.global_object()).associated_document();
                auto task = HTML::Task::create(vm(), HTML::Task::Source::DOMManipulation, &document,
                    GC::create_function(heap(), [this, finish_event]() {
                        dispatch_event(finish_event);
                    }));
                m_pending_finish_microtask_id = task->id();
                (void)HTML::main_thread_event_loop().task_queue().add(task);
            }
        });

        // 2. If synchronously notify is true, cancel any queued microtask to run the finish notification steps for this
        //    animation, and run the finish notification steps immediately.
        if (synchronously_notify == SynchronouslyNotify::Yes) {
            if (m_pending_finish_microtask_id.has_value()) {
                HTML::main_thread_event_loop().task_queue().remove_tasks_matching([id = move(m_pending_finish_microtask_id)](auto const& task) {
                    return task.id() == id;
                });
            }
            finish_notification_steps->function()();
        }
        //    Otherwise, if synchronously notify is false, queue a microtask to run finish notification steps for
        //    animation unless there is already a microtask queued to run those steps for animation.
        else if (!m_pending_finish_microtask_id.has_value()) {
            auto& document = as<HTML::Window>(realm.global_object()).associated_document();

            auto task = HTML::Task::create(vm(), HTML::Task::Source::DOMManipulation, &document, GC::create_function(heap(), [finish_notification_steps, &realm]() {
                HTML::TemporaryExecutionContext context { realm };
                finish_notification_steps->function()();
            }));

            m_pending_finish_microtask_id = task->id();
            (void)HTML::main_thread_event_loop().task_queue().add(move(task));
        }
    }

    // 6. If current finished state is false and animation’s current finished promise is already resolved, set
    //    animation’s current finished promise to a new promise in the relevant Realm of animation.
    if (!current_finished_state && m_is_finished) {
        m_current_finished_promise = WebIDL::create_promise(realm);
        m_is_finished = false;
    }

    invalidate_effect();
}

// https://www.w3.org/TR/web-animations-1/#animation-reset-an-animations-pending-tasks
void Animation::reset_an_animations_pending_tasks()
{
    auto& realm = this->realm();

    // 1. If animation does not have a pending play task or a pending pause task, abort this procedure.
    if (!pending())
        return;

    // 2. If animation has a pending play task, cancel that task.
    m_pending_play_task = TaskState::None;

    // 3. If animation has a pending pause task, cancel that task.
    m_pending_pause_task = TaskState::None;

    // 4. Apply any pending playback rate on animation.
    apply_any_pending_playback_rate();

    // 5. Reject animation’s current ready promise with a DOMException named "AbortError".
    auto dom_exception = WebIDL::AbortError::create(realm, "Animation was cancelled"_utf16);
    WebIDL::reject_promise(realm, current_ready_promise(), dom_exception);

    // 6. Set the [[PromiseIsHandled]] internal slot of animation’s current ready promise to true.
    WebIDL::mark_promise_as_handled(current_ready_promise());

    // 7. Let animation’s current ready promise be the result of creating a new resolved Promise object with value
    //    animation in the relevant Realm of animation.
    m_current_ready_promise = WebIDL::create_resolved_promise(realm, this);
}

// https://drafts.csswg.org/web-animations-2/#ready
bool Animation::is_ready() const
{
    // An animation is ready at the first moment where all of the following conditions are true:

    // FIXME: - the user agent has completed any setup required to begin the playback of each inclusive descendant of
    //          the animation’s associated effect including rendering the first frame of any keyframe effect or
    //          executing any custom effects associated with an animation effect

    // - the animation is associated with a timeline that is not inactive.
    if (!m_timeline || m_timeline->is_inactive())
        return false;

    // - the animation’s hold time or start time is resolved.
    if (!m_hold_time.has_value() && !m_start_time.has_value())
        return false;

    return true;
}

// Step 12 of https://www.w3.org/TR/web-animations-1/#playing-an-animation-section
void Animation::run_pending_play_task()
{
    // 1. Assert that at least one of animation’s start time or hold time is resolved.
    VERIFY(m_start_time.has_value() || m_hold_time.has_value());

    // 2. Let ready time be the time value of the timeline associated with animation at the moment when animation became
    //    ready.
    // FIXME: We can get a more accurate time here if we record the actual instant we became ready rather than waiting
    //        to try and run this task
    auto ready_time = m_timeline->current_time().value();

    // 3. Perform the steps corresponding to the first matching condition below, if any:

    // -> If animation’s hold time is resolved,
    if (m_hold_time.has_value()) {
        // 1. Apply any pending playback rate on animation.
        apply_any_pending_playback_rate();

        // 2. Let new start time be the result of evaluating ready time - hold time / playback rate for animation. If
        //    the playback rate is zero, let new start time be simply ready time.
        auto new_start_time = m_playback_rate != 0.0 ? ready_time - (m_hold_time.value() / m_playback_rate) : ready_time;

        // 3. Set the start time of animation to new start time.
        m_start_time = new_start_time;

        // 4. If animation’s playback rate is not 0, make animation’s hold time unresolved.
        if (m_playback_rate != 0.0)
            m_hold_time = {};
    }
    // -> If animation’s start time is resolved and animation has a pending playback rate,
    else if (m_start_time.has_value() && m_pending_playback_rate.has_value()) {
        // 1. Let current time to match be the result of evaluating (ready time - start time) × playback rate for
        //    animation.
        auto current_time_to_match = (ready_time - m_start_time.value()) * m_playback_rate;

        // 2. Apply any pending playback rate on animation.
        apply_any_pending_playback_rate();

        // 3. If animation’s playback rate is zero, let animation’s hold time be current time to match.
        if (m_playback_rate == 0.0)
            m_hold_time = current_time_to_match;

        // 4. Let new start time be the result of evaluating ready time - current time to match / playback rate for
        //    animation. If the playback rate is zero, let new start time be simply ready time.
        auto new_start_time = m_playback_rate != 0.0 ? ready_time - (current_time_to_match / m_playback_rate) : ready_time;

        // 5. Set the start time of animation to new start time.
        m_start_time = new_start_time;
    }

    // 4. Resolve animation’s current ready promise with animation.
    WebIDL::resolve_promise(realm(), current_ready_promise(), this);

    // 5. Run the procedure to update an animation’s finished state for animation with the did seek flag set to false,
    //    and the synchronously notify flag set to false.
    update_finished_state(DidSeek::No, SynchronouslyNotify::No);
}

bool Animation::is_ready_to_run_pending_pause_task() const
{
    // NB: Step 10 of the procedure to "pause an animation" requires us to schedule the pending pause task to run when
    //     the following conditions are true:

    // https://www.w3.org/TR/web-animations-1/#pause-an-animation
    // https://drafts.csswg.org/web-animations-2/#pausing-an-animation-section
    // FIXME: - the user agent has performed any processing necessary to suspend the playback of animation’s associated
    //       effect, if any.

    // - the animation is associated with a timeline that is not inactive.
    if (!m_timeline || m_timeline->is_inactive())
        return false;

    // - the animation has a resolved hold time or start time.
    if (!m_hold_time.has_value() && !m_start_time.has_value())
        return false;

    return true;
}

// Step 10 of https://www.w3.org/TR/web-animations-1/#pause-an-animation
void Animation::run_pending_pause_task()
{
    // 1. Let ready time be the time value of the timeline associated with animation at the moment when the user agent
    //    completed processing necessary to suspend playback of animation’s associated effect.
    // FIXME: We can get a more accurate time here if we record the actual instant the above is true rather than waiting
    //        for this task to run
    auto ready_time = m_timeline->current_time().value();

    // 2. If animation’s start time is resolved and its hold time is not resolved, let animation’s hold time be the
    //    result of evaluating (ready time - start time) × playback rate.
    // Note: The hold time might be already set if the animation is finished, or if the animation has a pending play
    //       task. In either case we want to preserve the hold time as we enter the paused state.
    if (m_start_time.has_value() && !m_hold_time.has_value())
        m_hold_time = (ready_time - m_start_time.value()) * m_playback_rate;

    // 3. Apply any pending playback rate on animation.
    apply_any_pending_playback_rate();

    // 4. Make animation’s start time unresolved.
    m_start_time = {};

    // 5. Resolve animation’s current ready promise with animation.
    WebIDL::resolve_promise(realm(), current_ready_promise(), this);

    // 6. Run the procedure to update an animation’s finished state for animation with the did seek flag set to false,
    //    and the synchronously notify flag set to false.
    update_finished_state(DidSeek::No, SynchronouslyNotify::No);
}

GC::Ref<WebIDL::Promise> Animation::current_ready_promise() const
{
    if (!m_current_ready_promise) {
        // The current ready promise is initially a resolved Promise created using the procedure to create a new
        // resolved Promise with the animation itself as its value and created in the relevant Realm of the animation.
        m_current_ready_promise = WebIDL::create_resolved_promise(realm(), this);
    }

    return *m_current_ready_promise;
}

GC::Ref<WebIDL::Promise> Animation::current_finished_promise() const
{
    if (!m_current_finished_promise) {
        // The current finished promise is initially a pending Promise object.
        m_current_finished_promise = WebIDL::create_promise(realm());
    }

    return *m_current_finished_promise;
}

void Animation::invalidate_effect()
{
    if (!m_effect)
        return;

    if (auto* target = m_effect->target())
        target->document().set_needs_animated_style_update();
}

Animation::Animation(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
    static unsigned int next_animation_list_order = 0;
    m_global_animation_list_order = next_animation_list_order++;
}

void Animation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Animation);
    Base::initialize(realm);
}

void Animation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_effect);
    visitor.visit(m_timeline);
    visitor.visit(m_current_ready_promise);
    visitor.visit(m_current_finished_promise);
    if (m_owning_element.has_value())
        m_owning_element->visit(visitor);
}

void Animation::finalize()
{
    Base::finalize();
    if (m_timeline)
        m_timeline->disassociate_with_animation(*this);
}

}
