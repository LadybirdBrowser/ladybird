/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <LibGC/Cell.h>
#include <LibWeb/Animations/KeyframeSetHandle.h>
#include <LibWeb/Animations/RustFFI.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

class CompositorAnimationEffectState;

}

namespace Web::Animations {

struct AnimationUpdateContext;

namespace Detail {

struct AdoptRecord { };

}

// A reference to a record, which Rust owns and keeps alive for as long as anything references it.
template<typename Self, typename Record, void (*retain)(Record const*), void (*release)(Record const*)>
class RecordRef {
public:
    // Takes a reference of its own to a record something else holds one to.
    explicit RecordRef(Record const& record)
        : m_record(&record)
    {
        retain(m_record);
    }

    // Takes over a reference Rust handed out.
    RecordRef(Detail::AdoptRecord, Record const& record)
        : m_record(&record)
    {
    }

    static Self adopt(Record const& record) { return Self { Detail::AdoptRecord {}, record }; }

    RecordRef(RecordRef const& other)
        : RecordRef(*other.m_record)
    {
    }

    RecordRef(RecordRef&& other)
        : m_record(exchange(other.m_record, nullptr))
    {
    }

    RecordRef& operator=(RecordRef const& other)
    {
        if (this != &other)
            *this = RecordRef { *other.m_record };
        return *this;
    }

    RecordRef& operator=(RecordRef&& other)
    {
        if (this != &other) {
            drop();
            m_record = exchange(other.m_record, nullptr);
        }
        return *this;
    }

    ~RecordRef() { drop(); }

    Record const& record() const { return *m_record; }

    bool operator==(RecordRef const& other) const { return m_record == other.m_record; }

private:
    void drop()
    {
        if (auto const* record = exchange(m_record, nullptr))
            release(record);
    }

    Record const* m_record { nullptr };
};

class EffectRef;

// A reference to the record of an animation.
class AnimationRef : public RecordRef<AnimationRef, RustFFI::AnimationCell, RustFFI::animations_animation_retain, RustFFI::animations_animation_unretain> {
public:
    using RecordRef::RecordRef;

    // Visits what keeps the elements the record names alive: the animation's Animation, or, for an animation without
    // one, those elements. A holder the GC traces visits this.
    void visit_edges(GC::Cell::Visitor&) const;

    // https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    Optional<EffectRef> effect() const;

    // https://www.w3.org/TR/web-animations-1/#animation-relevant
    bool is_relevant() const;

    // What the host's per-frame decisions read of the animation now.
    RustFFI::FfiAnimationTimingState timing_state() const { return RustFFI::animations_animation_timing_state(&record(), false, 0); }
    // The same, with the current time of the animation at `timestamp` too.
    RustFFI::FfiAnimationTimingState timing_state_at_timestamp(double timestamp) const { return RustFFI::animations_animation_timing_state(&record(), true, timestamp); }

    // Updates the animation as its timeline updated its current time.
    void update() const;
    // https://www.w3.org/TR/web-animations-1/#dom-animation-cancel
    // Cancels the animation, invalidating the style of its target where `invalidate`.
    void cancel(bool invalidate = true) const;
    // Runs the pending play task of the animation if it is ready.
    void run_pending_play_task_if_ready() const;
    // Disassociates the animation from the target of its associated effect where it is inert, where a procedure
    // scheduled that.
    void disassociate_from_target_if_inert() const;

    // https://www.w3.org/TR/web-animations-1/#animation-composite-order
    static i32 composite_order(AnimationRef const&, AnimationRef const&);
};

// A reference to the record of an animation effect.
class EffectRef : public RecordRef<EffectRef, RustFFI::AnimationEffectCell, RustFFI::animations_effect_retain, RustFFI::animations_effect_release> {
public:
    using RecordRef::RecordRef;

    // A new KeyframeEffect's state, with the default timing, no keyframes and no target, as a CSS animation or
    // transition creates its associated effect.
    static EffectRef create_keyframe_effect();

    // Visits what keeps the element the record targets alive: the effect's AnimationEffect, or, for an effect without
    // one, that element. A holder the GC traces visits this.
    void visit_edges(GC::Cell::Visitor&) const;

    RustFFI::AnimationEffectId id() const { return RustFFI::animations_effect_id(&record()); }

    // https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    // The animation whose associated effect this effect is.
    Optional<AnimationRef> associated_animation() const;

    // https://www.w3.org/TR/web-animations-1/#effect-target
    DOM::Element* target() const { return static_cast<DOM::Element*>(RustFFI::animations_effect_target(&record()).element); }
    enum class InvalidateEffect : bool {
        No,
        Yes,
    };
    // Sets the target element, and where `invalidate`, notes what the keyframes read of it and invalidates the style
    // the effect produces.
    void set_target(DOM::Element*, InvalidateEffect = InvalidateEffect::Yes) const;
    void set_target(DOM::AbstractElement, InvalidateEffect = InvalidateEffect::Yes) const;
    void set_pseudo_element(Optional<CSS::PseudoElement>) const;
    Optional<CSS::PseudoElement> pseudo_element_type() const;
    Optional<DOM::AbstractElement> target_abstract_element() const;

    // The keyframes the effect runs, which the view borrows until they change.
    KeyframeSetView keyframe_set() const { return KeyframeSetView { RustFFI::animations_effect_keyframe_set(&record()) }; }
    // https://www.w3.org/TR/web-animations-1/#target-property
    // The properties the effect's keyframes animate, borrowed until its keyframes change.
    TargetProperties target_properties() const { return keyframe_set().target_properties(); }
    bool composite_is_replace() const { return RustFFI::animations_effect_composite(&record()) == CSS::StyleValueFFI::FfiCompositeOperation::Replace; }
    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-composite
    void set_composite(CSS::StyleValueFFI::FfiCompositeOperation) const;
    // Sets the keyframes the effect runs, invalidating the style it produces where they change.
    void set_keyframe_set(KeyframeSetHandle) const;
    // The normalized timing the timing model reads of the effect.
    RustFFI::AnimationEffectTiming timing() const { return RustFFI::animations_effect_timing(&record()); }
    // The descriptor reads the timing function's control points from a copy in storage the caller keeps alive.
    CSS::EasingFunction::Descriptor timing_function_with_points_in(Vector<CSS::EasingFunction::LinearPoint>& storage) const;

    // The timing of the effect at its local time, the current time of its associated animation.
    RustFFI::FfiResolvedEffectTiming resolve_timing() const;
    void set_previous_phase_and_current_iteration(RustFFI::AnimationEffectPhase phase, double current_iteration) const { RustFFI::animations_effect_set_previous_phase_and_current_iteration(&record(), phase, current_iteration); }

    bool has_flag(RustFFI::EffectFlag flag) const { return RustFFI::animations_effect_has_flag(&record(), flag); }
    bool set_flag(RustFFI::EffectFlag flag, bool value) const { return RustFFI::animations_effect_set_flag(&record(), flag, value); }
    bool is_compositor_driven() const { return has_flag(RustFFI::EffectFlag::CompositorDriven); }
    bool is_compositor_replaced() const { return has_flag(RustFFI::EffectFlag::CompositorReplaced); }
    bool is_offscreen_throttled() const { return has_flag(RustFFI::EffectFlag::OffscreenThrottled); }
    bool is_observation_relevant_compositor_animation() const { return has_flag(RustFFI::EffectFlag::ObservationRelevantCompositorAnimation); }
    bool per_frame_animation_tick_was_skipped() const { return has_flag(RustFFI::EffectFlag::PerFrameAnimationTickWasSkipped); }

    // Whether a compositor animation may run every property the effect targets.
    bool may_run_on_the_compositor() const { return RustFFI::animations_effect_may_run_on_the_compositor(&record()); }
    // The compositor animations built from this effect live in its record; the state is made when first needed.
    Painting::CompositorAnimationEffectState compositor_animation_state() const;
    Optional<Painting::CompositorAnimationEffectState> existing_compositor_animation_state() const;
    bool has_retained_compositor_animations() const;

    // Invalidates the style the effect produces, forgetting what the host concluded of it.
    void invalidate() const;

    bool can_skip_per_frame_style_update() const;
    bool can_skip_per_frame_animation_tick() const;
    void request_observation_sample() const;
    // Requests an observation sample on behalf of a read of an element, unless one was requested in the same task.
    bool request_element_scoped_observation_sample(u64 task_generation) const;
    // Samples the effect into the style of its target for the update.
    void update_computed_properties(AnimationUpdateContext&) const;

private:
    void note_observation_sample_requested() const;
    bool may_animate_display() const;
    bool css_animation_events_are_heard(RustFFI::FfiAnimationTimingState const&, bool only_iteration_events) const;
};

inline Optional<EffectRef> AnimationRef::effect() const
{
    if (auto const* effect = RustFFI::animations_animation_effect(&record()))
        return EffectRef { *effect };
    return {};
}

inline Optional<AnimationRef> EffectRef::associated_animation() const
{
    if (auto const* animation = RustFFI::animations_effect_associated_animation(&record()))
        return AnimationRef { *animation };
    return {};
}

// Visits a GC cell an animation record names, as a holder of the record the GC traces does.
void visit_host_cell(void* visitor, void* cell, RustFFI::HostCellKind);

// Keeps an animation a procedure hands the host alive, as an AnimationRef appended to the GC::RootVector<AnimationRef>
// `root_vector`, for as long as that lives.
void keep_animation_alive_in(void* root_vector, RustFFI::AnimationCell const*);

// The relevant global object of an animation: that of its object, or, for an animation that has none, that of the
// node document of the element that owns it or whose animation element it is.
JS::Object& relevant_global_object_of(AnimationRef const&);

// Performs what a procedure of an animation asked the host to do, which the host hands to the procedures to run. Only
// settling the promises of the animation and queueing its playback events need its object, and an animation that has
// none has nothing to settle them for or to dispatch them at.
void perform_host_effect(RustFFI::FfiHostEffect const*);

// Creates the playback event the pending animation event queue of `document` holds of `animation`, and dispatches it
// at the object of the animation.
void dispatch_pending_playback_event(AnimationRef const&, DOM::Document const&, RustFFI::FfiPendingAnimationEvent const&);

// https://www.w3.org/TR/css-animations-2/#owning-element-section
void set_owning_element(AnimationRef const&, DOM::AbstractElement);

// The bytes of the objects script sees of an animation and of its associated effect, beside their records.
size_t object_memory_size(AnimationRef const&);

// Compares two DOM::Elements in tree order, as the composite order of their animations does.
i32 compare_elements_in_tree_order_for_composite_order(void* first, void* second);

// A set of animation records that keeps none of them alive, which Rust owns. The record of an animation that dies
// leaves the set as the set is next enumerated.
class WeakAnimationSetView {
public:
    explicit WeakAnimationSetView(RustFFI::WeakAnimationSet& set)
        : m_set(&set)
    {
    }

    bool set(AnimationRef const& animation) { return RustFFI::animations_weak_animation_set_insert(m_set, &animation.record()); }
    bool remove(RustFFI::AnimationCell const& animation) { return RustFFI::animations_weak_animation_set_remove(m_set, &animation); }
    bool contains(RustFFI::AnimationCell const& record) const { return RustFFI::animations_weak_animation_set_contains(m_set, &record); }
    bool is_empty() const { return begin() == end(); }

    struct EndIterator { };

    class Iterator {
    public:
        AnimationRef const& operator*() const { return *m_current; }
        AnimationRef const* operator->() const { return &*m_current; }

        Iterator& operator++()
        {
            ++m_index;
            settle();
            return *this;
        }

        bool operator==(EndIterator) const { return !m_current.has_value(); }

    private:
        friend class WeakAnimationSetView;

        explicit Iterator(RustFFI::WeakAnimationSet const* set)
            : m_set(set)
        {
            settle();
        }

        // Moves on to the first entry from the current one whose animation is alive.
        void settle()
        {
            m_current.clear();
            for (; m_index < RustFFI::animations_weak_animation_set_entry_count(m_set); ++m_index) {
                if (auto const* record = RustFFI::animations_weak_animation_set_get(m_set, m_index)) {
                    m_current = AnimationRef::adopt(*record);
                    return;
                }
            }
        }

        RustFFI::WeakAnimationSet const* m_set { nullptr };
        size_t m_index { 0 };
        Optional<AnimationRef> m_current;
    };

    Iterator begin() const
    {
        RustFFI::animations_weak_animation_set_prune(m_set);
        return Iterator { m_set };
    }
    EndIterator end() const { return {}; }

protected:
    RustFFI::WeakAnimationSet* m_set { nullptr };
};

}
