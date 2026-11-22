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

    // https://www.w3.org/TR/web-animations-1/#play-states
    RustFFI::AnimationPlayState play_state() const { return RustFFI::animations_animation_play_state(&record()); }

    // https://www.w3.org/TR/web-animations-1/#playback-rate
    double playback_rate() const { return RustFFI::animations_animation_playback_rate(&record()); }
};

// A reference to the record of an animation effect.
class EffectRef : public RecordRef<EffectRef, RustFFI::AnimationEffectCell, RustFFI::animations_effect_retain, RustFFI::animations_effect_release> {
public:
    using RecordRef::RecordRef;

    // Visits what keeps the element the record targets alive: the effect's AnimationEffect, or, for an effect without
    // one, that element. A holder the GC traces visits this.
    void visit_edges(GC::Cell::Visitor&) const;

    RustFFI::AnimationEffectId id() const { return RustFFI::animations_effect_id(&record()); }

    // https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    // The animation whose associated effect this effect is.
    Optional<AnimationRef> associated_animation() const;

    // https://www.w3.org/TR/web-animations-1/#effect-target
    DOM::Element* target() const { return static_cast<DOM::Element*>(RustFFI::animations_effect_target(&record()).element); }
    Optional<CSS::PseudoElement> pseudo_element_type() const;

    // The keyframes the effect runs, which the view borrows until they change.
    KeyframeSetView keyframe_set() const { return KeyframeSetView { RustFFI::animations_effect_keyframe_set(&record()) }; }
    // https://www.w3.org/TR/web-animations-1/#target-property
    // The properties the effect's keyframes animate, borrowed until its keyframes change.
    TargetProperties target_properties() const { return keyframe_set().target_properties(); }
    bool has_flag(RustFFI::EffectFlag flag) const { return RustFFI::animations_effect_has_flag(&record(), flag); }
    bool is_compositor_driven() const { return has_flag(RustFFI::EffectFlag::CompositorDriven); }
    bool is_compositor_replaced() const { return has_flag(RustFFI::EffectFlag::CompositorReplaced); }

    // Invalidates the style the effect produces, forgetting what the host concluded of it.
    void invalidate() const;

    bool can_skip_per_frame_style_update() const { return RustFFI::animations_effect_can_skip_per_frame_style_update(&record()); }
    // Samples the effect into the style of its target for the update.
    void update_computed_properties(AnimationUpdateContext&) const;
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
// Runs `run` with `run_context` in an execution context of the relevant realm of `animation`.
void in_execution_context_of_animation(void*, RustFFI::AnimationCell const* animation, void (*run)(void*), void* run_context);

// The relevant global object of an animation: that of its object, or, for an animation that has none, that of the
// node document of the element that owns it or whose animation element it is.
JS::Object& relevant_global_object_of(AnimationRef const&);

// Performs what a procedure of an animation asked the host to do, which the host hands to the procedures to run. Only
// settling the promises of the animation and queueing its playback events need its object, and an animation that has
// none has nothing to settle them for or to dispatch them at. Bindings/Rust/src/animations calls it too.
extern "C" void web_animation_perform_host_effect(RustFFI::FfiHostEffect const*);

// Compares two DOM::Elements in tree order, as the composite order of their animations does.
i32 compare_elements_in_tree_order_for_composite_order(void* first, void* second);

// A view of the record of a timeline associated with a document, which the document keeps alive for as long as it
// lives.
class TimelineView {
public:
    explicit TimelineView(RustFFI::TimelineCell const& record)
        : m_record(&record)
    {
    }

    RustFFI::TimelineCell const& record() const { return *m_record; }

    RustFFI::FfiTimeline read() const { return RustFFI::animations_timeline_read(m_record); }
    Optional<RustFFI::TimeValue> current_time() const
    {
        auto current_time = read().current_time;
        if (!current_time.has_value)
            return {};
        return current_time.value;
    }
    // A scroll timeline, or a view timeline, which is one.
    bool is_scroll_timeline() const
    {
        auto kind = read().kind;
        return kind == RustFFI::FfiTimelineKind::Scroll || kind == RustFFI::FfiTimelineKind::View;
    }
    bool is_view_timeline() const { return read().kind == RustFFI::FfiTimelineKind::View; }

    bool operator==(TimelineView const& other) const { return m_record == other.m_record; }

private:
    RustFFI::TimelineCell const* m_record { nullptr };
};

// A set of animation records that keeps none of them alive, which Rust owns. The record of an animation that dies
// leaves the set as the set is next enumerated.
class WeakAnimationSetView {
public:
    explicit WeakAnimationSetView(RustFFI::WeakAnimationSet& set)
        : m_set(&set)
    {
    }

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

    bool contains(AnimationRef const& animation) const { return RustFFI::animations_weak_animation_set_contains(m_set, &animation.record()); }

protected:
    RustFFI::WeakAnimationSet* m_set { nullptr };
};

// NB: The animations associated with a timeline, none of which it keeps alive.
inline WeakAnimationSetView associated_animations_of(TimelineView timeline)
{
    return WeakAnimationSetView { *RustFFI::animations_timeline_associated_animations(&timeline.record()) };
}

// NB: The animations associated with a timeline that an update of its current time may change, none of which it keeps
//     alive. The others have finished on a monotonically increasing timeline and have no pending task. This includes
//     every running animation of the timeline, and may include some that are no longer associated with it, until the
//     timeline next updates.
inline WeakAnimationSetView animations_needing_update_of(TimelineView timeline)
{
    return WeakAnimationSetView { *RustFFI::animations_timeline_animations_needing_update(&timeline.record()) };
}

// NB: The animations associated with the elements of a document, none of which it keeps alive.
inline WeakAnimationSetView associated_animations_of(RustFFI::DocumentAnimations& animations)
{
    return WeakAnimationSetView { *RustFFI::animations_document_associated_animations(&animations) };
}

}
