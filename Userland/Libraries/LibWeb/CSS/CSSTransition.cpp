/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSTransitionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSTransition);

JS::NonnullGCPtr<CSSTransition> CSSTransition::create(JS::Realm& realm, PropertyID property_id, size_t transition_generation)
{
    return realm.heap().allocate<CSSTransition>(realm, realm, property_id, transition_generation);
}

Animations::AnimationClass CSSTransition::animation_class() const
{
    return Animations::AnimationClass::CSSTransition;
}

Optional<int> CSSTransition::class_specific_composite_order(JS::NonnullGCPtr<Animations::Animation> other_animation) const
{
    auto other = JS::NonnullGCPtr { verify_cast<CSSTransition>(*other_animation) };

    // Within the set of CSS Transitions, two animations A and B are sorted in composite order (first to last) as
    // follows:

    // 1. If neither A nor B has an owning element, sort based on their relative position in the global animation list.
    if (!owning_element() && !other->owning_element())
        return global_animation_list_order() - other->global_animation_list_order();

    // 2. Otherwise, if only one of A or B has an owning element, let the animation with an owning element sort first.
    if (owning_element() && !other->owning_element())
        return -1;
    if (!owning_element() && other->owning_element())
        return 1;

    // 3. Otherwise, if the owning element of A and B differs, sort A and B by tree order of their corresponding owning
    //    elements. With regard to pseudo-elements, the sort order is as follows:
    //    - element
    //    - ::marker
    //    - ::before
    //    - any other pseudo-elements not mentioned specifically in this list, sorted in ascending order by the Unicode
    //      codepoints that make up each selector
    //    - ::after
    //    - element children
    if (owning_element().ptr() != other->owning_element().ptr()) {
        // FIXME: Actually sort by tree order
        return {};
    }

    // 4. Otherwise, if A and B have different transition generation values, sort by their corresponding transition
    //    generation in ascending order.
    if (m_transition_generation != other->m_transition_generation)
        return m_transition_generation - other->m_transition_generation;

    // FIXME:
    // 5. Otherwise, sort A and B in ascending order by the Unicode codepoints that make up the expanded transition
    //    property name of each transition (i.e. without attempting case conversion and such that ‘-moz-column-width’
    //    sorts before ‘column-width’).
    return {};
}

CSSTransition::CSSTransition(JS::Realm& realm, PropertyID property_id, size_t transition_generation)
    : Animations::Animation(realm)
    , m_transition_property(property_id)
    , m_transition_generation(transition_generation)
{
    // FIXME:
    // Transitions generated using the markup defined in this specification are not added to the global animation list
    // when they are created. Instead, these animations are appended to the global animation list at the first moment
    // when they transition out of the idle play state after being disassociated from their owning element. Transitions
    // that have been disassociated from their owning element but are still idle do not have a defined composite order.
}

void CSSTransition::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSTransition);
}

void CSSTransition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_declaration);
}

}
