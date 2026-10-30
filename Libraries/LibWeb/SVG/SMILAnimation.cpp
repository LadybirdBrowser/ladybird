/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/SMILAnimation.h>
#include <LibWeb/SVG/SMILTimeContainer.h>
#include <LibWeb/SVG/SVGAnimationElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SMILAnimation);

GC::Ref<SMILAnimation> SMILAnimation::create(SVGAnimationElement& animation_element, SMILTimeContainer& time_container, double begin_time)
{
    auto& environment = animation_element.document().relevant_settings_object();
    return GC::Heap::the().allocate<SMILAnimation>(environment, animation_element, time_container, begin_time);
}

SMILAnimation::SMILAnimation(HTML::EnvironmentSettingsObject& environment, SVGAnimationElement& animation_element, SMILTimeContainer& time_container, double begin_time)
    : Animations::Animation(environment)
    , m_animation_element(animation_element)
    , m_time_container(time_container)
    , m_begin_time(begin_time)
{
}

void SMILAnimation::cancel(ShouldInvalidate should_invalidate)
{
    m_time_container->remove_animation(*this);
    Base::cancel(should_invalidate);
}

// https://www.w3.org/TR/smil-animation/#AnimationSandwichModel
int SMILAnimation::class_specific_composite_order(GC::Ref<Animations::Animation> other) const
{
    // Within an OM, animations are prioritized according to when each begins. The animation first begun has lowest
    // priority and the most recently begun animation has highest priority. When two animations start at the same
    // moment in time, the activation order is resolved as follows:
    // ...
    // If two animations share no time dependency relationship (e.g., neither is defined relative to the other, even
    // indirectly) the element that appears first in the document has lower priority.
    auto const& other_animation = as<SMILAnimation>(*other);
    if (m_begin_time != other_animation.m_begin_time)
        return m_begin_time < other_animation.m_begin_time ? -1 : 1;
    // NB: Begin times only come from offsets and DOM method calls for now, so no animation is defined relative to
    //     another.
    auto const& other_element = other_animation.animation_element();
    if (m_animation_element.ptr() == &other_element)
        return 0;
    return m_animation_element->is_before(other_element) ? -1 : 1;
}

void SMILAnimation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation_element);
    visitor.visit(m_time_container);
}

}
