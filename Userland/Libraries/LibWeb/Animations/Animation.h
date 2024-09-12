/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/AnimationPrototype.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::Animations {

// Sorted by composite order:
// https://www.w3.org/TR/css-animations-2/#animation-composite-order
enum class AnimationClass {
    CSSAnimationWithOwningElement,
    CSSTransition,
    CSSAnimationWithoutOwningElement,
    None,
};

// https://www.w3.org/TR/web-animations-1/#the-animation-interface
class Animation : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Animation, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(Animation);

public:
    static JS::NonnullGCPtr<Animation> create(JS::Realm&, JS::GCPtr<AnimationEffect>, Optional<JS::GCPtr<AnimationTimeline>>);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Animation>> construct_impl(JS::Realm&, JS::GCPtr<AnimationEffect>, Optional<JS::GCPtr<AnimationTimeline>>);

    FlyString const& id() const { return m_id; }
    void set_id(FlyString value) { m_id = move(value); }

    JS::GCPtr<AnimationEffect> effect() const { return m_effect; }
    void set_effect(JS::GCPtr<AnimationEffect>);

    JS::GCPtr<AnimationTimeline> timeline() const { return m_timeline; }
    void set_timeline(JS::GCPtr<AnimationTimeline>);

    Optional<double> const& start_time() const { return m_start_time; }
    void set_start_time(Optional<double> const&);

    Optional<double> current_time() const;
    WebIDL::ExceptionOr<void> set_current_time(Optional<double> const&);

    double playback_rate() const { return m_playback_rate; }
    WebIDL::ExceptionOr<void> set_playback_rate(double value);

    Bindings::AnimationPlayState play_state() const;

    bool is_relevant() const;

    bool is_replaceable() const;
    Bindings::AnimationReplaceState replace_state() const { return m_replace_state; }
    void set_replace_state(Bindings::AnimationReplaceState value);

    // https://www.w3.org/TR/web-animations-1/#dom-animation-pending
    bool pending() const { return m_pending_play_task == TaskState::Scheduled || m_pending_pause_task == TaskState::Scheduled; }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-ready
    JS::NonnullGCPtr<JS::Object> ready() const { return *current_ready_promise()->promise(); }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-finished
    JS::NonnullGCPtr<JS::Object> finished() const { return *current_finished_promise()->promise(); }
    bool is_finished() const { return m_is_finished; }

    JS::GCPtr<WebIDL::CallbackType> onfinish();
    void set_onfinish(JS::GCPtr<WebIDL::CallbackType>);
    JS::GCPtr<WebIDL::CallbackType> oncancel();
    void set_oncancel(JS::GCPtr<WebIDL::CallbackType>);
    JS::GCPtr<WebIDL::CallbackType> onremove();
    void set_onremove(JS::GCPtr<WebIDL::CallbackType>);

    enum class AutoRewind {
        Yes,
        No,
    };
    enum class ShouldInvalidate {
        Yes,
        No,
    };
    void cancel(ShouldInvalidate = ShouldInvalidate::Yes);
    WebIDL::ExceptionOr<void> finish();
    WebIDL::ExceptionOr<void> play();
    WebIDL::ExceptionOr<void> play_an_animation(AutoRewind);
    WebIDL::ExceptionOr<void> pause();
    WebIDL::ExceptionOr<void> update_playback_rate(double);
    WebIDL::ExceptionOr<void> reverse();
    void persist();

    Optional<double> convert_an_animation_time_to_timeline_time(Optional<double>) const;
    Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<double>) const;

    JS::GCPtr<DOM::Document> document_for_timing() const;
    void notify_timeline_time_did_change();

    void effect_timing_changed(Badge<AnimationEffect>);

    virtual bool is_css_animation() const { return false; }
    virtual bool is_css_transition() const { return false; }

    JS::GCPtr<DOM::Element> owning_element() const { return m_owning_element; }
    void set_owning_element(JS::GCPtr<DOM::Element> value) { m_owning_element = value; }

    virtual AnimationClass animation_class() const { return AnimationClass::None; }
    virtual Optional<int> class_specific_composite_order(JS::NonnullGCPtr<Animation>) const { return {}; }

    unsigned int global_animation_list_order() const { return m_global_animation_list_order; }

    auto release_saved_cancel_time() { return move(m_saved_cancel_time); }

protected:
    Animation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    enum class TaskState {
        None,
        Scheduled,
    };

    enum class DidSeek {
        Yes,
        No,
    };

    enum class SynchronouslyNotify {
        Yes,
        No,
    };

    double associated_effect_end() const;
    double effective_playback_rate() const;

    void apply_any_pending_playback_rate();
    WebIDL::ExceptionOr<void> silently_set_current_time(Optional<double>);
    void update_finished_state(DidSeek, SynchronouslyNotify);
    void reset_an_animations_pending_tasks();

    void run_pending_play_task();
    void run_pending_pause_task();

    JS::NonnullGCPtr<WebIDL::Promise> current_ready_promise() const;
    JS::NonnullGCPtr<WebIDL::Promise> current_finished_promise() const;

    void invalidate_effect();

    // https://www.w3.org/TR/web-animations-1/#dom-animation-id
    FlyString m_id;

    // https://www.w3.org/TR/web-animations-1/#global-animation-list
    unsigned int m_global_animation_list_order { 0 };

    // https://www.w3.org/TR/web-animations-1/#dom-animation-effect
    JS::GCPtr<AnimationEffect> m_effect;

    // https://www.w3.org/TR/web-animations-1/#dom-animation-timeline
    JS::GCPtr<AnimationTimeline> m_timeline;

    // https://www.w3.org/TR/web-animations-1/#animation-start-time
    Optional<double> m_start_time {};

    // https://www.w3.org/TR/web-animations-1/#animation-hold-time
    Optional<double> m_hold_time {};

    // https://www.w3.org/TR/web-animations-1/#previous-current-time
    Optional<double> m_previous_current_time {};

    // https://www.w3.org/TR/web-animations-1/#playback-rate
    double m_playback_rate { 1.0 };

    // https://www.w3.org/TR/web-animations-1/#pending-playback-rate
    Optional<double> m_pending_playback_rate {};

    // https://www.w3.org/TR/web-animations-1/#dom-animation-replacestate
    Bindings::AnimationReplaceState m_replace_state { Bindings::AnimationReplaceState::Active };

    // Note: The following promises are initialized lazily to avoid constructing them outside of an execution context
    // https://www.w3.org/TR/web-animations-1/#current-ready-promise
    mutable JS::GCPtr<WebIDL::Promise> m_current_ready_promise;

    // https://www.w3.org/TR/web-animations-1/#current-finished-promise
    mutable JS::GCPtr<WebIDL::Promise> m_current_finished_promise;
    bool m_is_finished { false };

    // https://www.w3.org/TR/web-animations-1/#pending-play-task
    TaskState m_pending_play_task { TaskState::None };

    // https://www.w3.org/TR/web-animations-1/#pending-pause-task
    TaskState m_pending_pause_task { TaskState::None };

    // https://www.w3.org/TR/css-animations-2/#owning-element-section
    JS::GCPtr<DOM::Element> m_owning_element;

    Optional<HTML::TaskID> m_pending_finish_microtask_id;

    Optional<double> m_saved_play_time;
    Optional<double> m_saved_pause_time;
    Optional<double> m_saved_cancel_time;
};

}
