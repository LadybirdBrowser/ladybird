/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/Animations/RustFFI.h>
#include <LibWeb/Bindings/Animatable.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/Export.h>

namespace Web::Animations {

// One of the lists of animations an element holds, which a loop over it reads entry by entry as it goes, so the loop
// may change the list.
class ElementAnimationList {
public:
    ElementAnimationList(RustFFI::ElementAnimations const* animations, RustFFI::ElementAnimationListKind kind, RustFFI::AnimationOwnerSlot slot)
        : m_animations(animations)
        , m_kind(kind)
        , m_slot(slot)
    {
    }

    size_t size() const { return m_animations ? RustFFI::animations_element_animations_count(m_animations, m_kind, m_slot) : 0; }
    bool is_empty() const { return size() == 0; }
    AnimationRef at(size_t index) const { return AnimationRef { *RustFFI::animations_element_animations_get(m_animations, m_kind, m_slot, index) }; }

    struct End { };
    class Iterator {
    public:
        Iterator(ElementAnimationList const& list)
            : m_list(list)
        {
        }
        AnimationRef operator*() const { return m_list.at(m_index); }
        Iterator& operator++()
        {
            ++m_index;
            return *this;
        }
        bool operator!=(End) const { return m_index < m_list.size(); }

    private:
        ElementAnimationList const& m_list;
        size_t m_index { 0 };
    };
    Iterator begin() const { return Iterator { *this }; }
    End end() const { return {}; }

private:
    RustFFI::ElementAnimations const* m_animations;
    RustFFI::ElementAnimationListKind m_kind;
    RustFFI::AnimationOwnerSlot m_slot;
};

// https://drafts.csswg.org/web-animations-1/#animatable
class WEB_API Animatable {
public:
    virtual ~Animatable();

    ElementAnimationList associated_animations_in_composite_order();
    ElementAnimationList associated_animations_unordered() const;
    void invalidate_associated_animation_composite_order();
    bool has_associated_animations() const;
    // The element's lists of its animations, where it holds any.
    RustFFI::ElementAnimations const* element_animations_if_created() const { return m_animations; }
    // The element's lists of its animations, created where it holds none yet, which also follow what the element
    // published of the effects that target it to the style engine.
    RustFFI::ElementAnimations& ensure_animations();
    // Forgets what the element published to the style engine of the effects that target it, as its identity there
    // changes.
    void forget_published_effect_descriptions();
    // The bytes of the element's lists of its animations and transitions, with the records of the animations associated
    // with it and of their effects, and of the keyframe sets those run that `first_counted` answers are counted first.
    size_t animation_record_memory_size(bool (*first_counted)(void*, RustFFI::KeyframeSet const*), void* context) const;

    void associate_with_animation(AnimationRef const&);
    // Disassociates the animation from the element, which `effect`, the animation's associated effect, targeted.
    void disassociate_with_animation(AnimationRef const&, Optional<EffectRef> const& effect);
    void on_document_changed(DOM::Document& old_document, DOM::Document& new_document);
    void cancel_css_animations_and_transitions();
    // Cancels the CSS animations and transitions one of the element's pseudo-elements owns.
    void cancel_css_animations_and_transitions_of(CSS::PseudoElement);
    // Plays or pauses the CSS animations the element and its pseudo-elements own as their animation-play-state last
    // computed.
    void play_or_pause_css_animations_as_their_play_state_asks();

    // The list of the element, or of one of its known pseudo-elements, of the CSS animations and transitions it owns.
    static Optional<RustFFI::AnimationOwnerSlot> owner_slot(Optional<CSS::PseudoElement>);

    bool has_css_animations_or_transitions() const;
    bool has_css_defined_animations(Optional<CSS::PseudoElement>) const;

    // Whether the installed style gives any longhand a matching transition-property entry, answered without listing them.
    bool has_matching_transition_property_entry(Optional<CSS::PseudoElement>) const;
    bool has_existing_transitions(Optional<CSS::PseudoElement>) const;

protected:
    void visit_edges(JS::Cell::Visitor&);
    // Lets go of the animations as the GC finalizes the element, so that an animation that only the lists held is no
    // longer alive to the host before anything else runs.
    void finalize_animations();

private:
    // The lists, created as the element first holds an animation, which the element owns.
    RustFFI::ElementAnimations* m_animations { nullptr };
};

}
