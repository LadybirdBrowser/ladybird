/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Animations/KeyframeSetHandle.h>
#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/Animations/RustFFI.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/InstalledStyle.h>
#include <LibWeb/CSS/PropertyNameAndID.h>

namespace Web::Animations {

// This object lives for the duration of an animation update, and is used to store per-element data about animated CSS properties.
struct AnimationUpdateContext {
    struct ElementData {
        ElementData();
        ElementData(CSS::InstalledStyle, RefPtr<CSS::ComputedStyleWorkingSet>);
        ElementData(ElementData&&);
        ElementData& operator=(ElementData&&);
        ~ElementData();

        // NB: The effects keep their AnimationEffects alive while the update holds them.
        void visit_edges(GC::Cell::Visitor&);

        CSS::InstalledStyle style_before_update;
        RefPtr<CSS::ComputedStyleWorkingSet> target_style;
        Vector<EffectRef> effects;
        // Whether the record the update samples over was computed by this style update, so that the base
        // adjustments the sampled values feed into were made over the values the element holds now.
        bool base_is_current { false };
    };

    AnimationUpdateContext();
    // Publishes what is left unpublished.
    ~AnimationUpdateContext();

    // Samples each element's effects over the style it holds and publishes what they compose, which the destructor
    // otherwise does.
    void publish();

    // Notes that the update samples `effect` over the style `abstract_element` holds.
    void add_effect(DOM::AbstractElement, EffectRef const& effect);

    // NOTE: This is lazily populated by KeyframeEffects as their respective animations are applied to an element.
    GC::RootHashMap<DOM::AbstractElement, ElementData> elements;
};

}
