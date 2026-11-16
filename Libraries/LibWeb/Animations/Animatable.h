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

namespace Web::CSS {

class CSSAnimation;
class CSSTransition;

}

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

    enum class IncludeSubtree {
        No,
        Yes
    };

    // The relevant animations of the element, and of its descendants with IncludeSubtree::Yes, in no particular order.
    void for_each_relevant_animation(IncludeSubtree, Function<void(AnimationRef const&)> const&) const;
    ElementAnimationList associated_animations_in_composite_order();
    ElementAnimationList associated_animations_unordered() const;
    void invalidate_associated_animation_composite_order();
    bool has_relevant_animations() const;
    bool has_associated_animations() const;
    // The element's lists of its animations, created where it holds none yet, which also follow what the element
    // published of the effects that target it to the style engine.
    RustFFI::ElementAnimations& ensure_animations();
    // Forgets what the element published to the style engine of the effects that target it, as its identity there
    // changes.
    void forget_published_effect_descriptions();
    // The bytes of the element's lists of its animations and transitions, beside the animations themselves.
    size_t animation_lists_memory_size() const;

    void associate_with_animation(AnimationRef const&);
    // Disassociates the animation from the element, which `effect`, the animation's associated effect, targeted.
    void disassociate_with_animation(AnimationRef const&, Optional<EffectRef> const& effect);
    void on_document_changed(DOM::Document& old_document, DOM::Document& new_document);
    void cancel_css_animations_and_transitions();
    // Plays or pauses the CSS animations the element and its pseudo-elements own as their animation-play-state last
    // computed.
    void play_or_pause_css_animations_as_their_play_state_asks();

    // Whether the element or the pseudo-element can own CSS animations and transitions, which only the known
    // pseudo-elements can.
    static bool can_own_css_animations_and_transitions(Optional<CSS::PseudoElement>);

    bool has_css_animations_or_transitions() const;
    bool has_css_defined_animations(Optional<CSS::PseudoElement>) const;
    GC::RootVector<AnimationRef> css_defined_animations(Optional<CSS::PseudoElement>) const;
    void set_css_defined_animations(Optional<CSS::PseudoElement>, ReadonlySpan<AnimationRef>);

    // Whether the installed style gives any longhand a matching transition-property entry, answered without listing them.
    bool has_matching_transition_property_entry(Optional<CSS::PseudoElement>) const;
    // Commits a transition started provisionally, whose target is now associated with it, as the transition of its
    // property the element or pseudo-element holds.
    void commit_provisional_transition(Optional<CSS::PseudoElement>, AnimationRef const&, RustFFI::TransitionOrigin);
    void remove_transition(Optional<CSS::PseudoElement>, CSS::PropertyNameAndID const&);
    bool has_existing_transitions(Optional<CSS::PseudoElement>) const;
    // The running or completed transition of each property the element or pseudo-element has.
    ElementAnimationList existing_transitions(Optional<CSS::PseudoElement>) const;
    Optional<AnimationRef> property_transition(Optional<CSS::PseudoElement>, CSS::PropertyNameAndID const&) const;

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
