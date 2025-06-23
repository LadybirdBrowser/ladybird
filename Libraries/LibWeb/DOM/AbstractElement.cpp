/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>

namespace Web::DOM {

AbstractElement::AbstractElement(GC::Ref<Element> element, Optional<CSS::PseudoElement> pseudo_element)
    : m_element(element)
    , m_pseudo_element(move(pseudo_element))
{
}

void AbstractElement::visit(GC::Cell::Visitor& visitor) const
{
    visitor.visit(m_element);
}

GC::Ptr<Layout::NodeWithStyle> AbstractElement::layout_node()
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element_node(*m_pseudo_element);
    return m_element->layout_node();
}

GC::Ptr<Element const> AbstractElement::parent_element() const
{
    if (m_pseudo_element.has_value())
        return m_element;
    return m_element->parent_element();
}

Optional<AbstractElement> AbstractElement::walk_layout_tree(WalkMethod walk_method)
{
    GC::Ptr<Layout::Node> node = layout_node();
    if (!node)
        return OptionalNone {};

    while (true) {
        switch (walk_method) {
        case WalkMethod::Previous:
            node = node->previous_in_pre_order();
            break;
        case WalkMethod::PreviousSibling:
            node = node->previous_sibling();
            break;
        }
        if (!node)
            return OptionalNone {};

        if (auto* previous_element = as_if<Element>(node->dom_node()))
            return AbstractElement { *previous_element };

        if (node->is_generated())
            return AbstractElement { *node->pseudo_element_generator(), node->generated_for_pseudo_element() };
    }
}

bool AbstractElement::is_before(AbstractElement const& other) const
{
    auto this_node = layout_node();
    auto other_node = other.layout_node();
    return this_node && other_node && this_node->is_before(*other_node);
}

GC::Ptr<CSS::ComputedProperties const> AbstractElement::computed_properties() const
{
    if (m_pseudo_element.has_value())
        return m_element->pseudo_element_computed_properties(*m_pseudo_element);
    return m_element->computed_properties();
}

bool AbstractElement::has_non_empty_counters_set() const
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element(*m_pseudo_element)->has_non_empty_counters_set();
    return m_element->has_non_empty_counters_set();
}

Optional<CSS::CountersSet&> AbstractElement::counters_set() const
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element(*m_pseudo_element)->counters_set();
    return m_element->counters_set();
}

CSS::CountersSet& AbstractElement::ensure_counters_set()
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element(*m_pseudo_element)->ensure_counters_set();
    return m_element->ensure_counters_set();
}

void AbstractElement::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    if (m_pseudo_element.has_value()) {
        m_element->get_pseudo_element(*m_pseudo_element)->set_counters_set(move(counters_set));
    } else {
        m_element->set_counters_set(move(counters_set));
    }
}

// https://drafts.csswg.org/css-lists-3/#instantiating-counters
void AbstractElement::update_initial_value_for_reversed_counter__after_increment(FlyString const& name, int amount) const
{
    auto counter = counters_set()->last_counter_with_name(name);

    VERIFY(counter.has_value());
    // Note: Only reversed counters can be instantiated without an initial value.
    VERIFY(counter->reversed);

    // https://drafts.csswg.org/css-lists-3/#instantiating-counters
    // For each element or pseudo-element el that increments or sets the same counter in the same scope:
    // 1. Let incrementNegated be el’s counter-increment integer value for this counter, multiplied by -1.
    auto const increment_negated = -amount;

    // 2. If first is true, then add incrementNegated to num and set first to false.
    if (!counter->value.has_value())
        counter->value = increment_negated;

    // 3. If el sets this counter with counter-set, then [...] break this loop. See below for the rest..
    if (counter->is_explicitly_set_reversed_counter)
        return;

    // 4. Add incrementNegated to num.
    counter->value->saturating_add(increment_negated);
}

// https://drafts.csswg.org/css-lists-3/#instantiating-counters
void AbstractElement::update_initial_value_for_reversed_counter__after_set(FlyString const& name, int amount) const
{
    auto counter = counters_set()->last_counter_with_name(name);

    VERIFY(counter.has_value());
    // Note: Only reversed counters can be instantiated without an initial value.
    VERIFY(counter->reversed);

    // For each element or pseudo-element el that increments or sets the same counter in the same scope:

    // 3. If el sets this counter with counter-set, then add that integer value to num and [...]
    counter->value->saturating_add(amount);
}

// https://drafts.csswg.org/css-lists-3/#auto-numbering
void AbstractElement::resolve_counters()
{
    // Resolving counter values on a given element is a multi-step process:
    auto const& style = *computed_properties();

    // 1. Existing counters are inherited from previous elements.
    inherit_counters();

    // https://drafts.csswg.org/css-lists-3/#counters-without-boxes
    // An element that does not generate a box (for example, an element with display set to none,
    // or a pseudo-element with content set to none) cannot set, reset, or increment a counter.
    // The counter properties are still valid on such an element, but they must have no effect.
    if (style.display().is_none())
        return;

    // 2. New counters are instantiated (counter-reset).
    auto counter_reset = style.counter_data(CSS::PropertyID::CounterReset);
    for (auto const& counter : counter_reset)
        // NOTE: The spec is ambiguous about initial values for reversed counters (see https://github.com/w3c/csswg-drafts/issues/6231)
        //       - Chromium (136) does not support reversed counters.
        //       - Firefox (138) treats a reversed counter with a value as if `reversed=false`. We do the same below.
        ensure_counters_set().instantiate_a_counter(counter.name, *this, counter.is_reversed && !counter.value.has_value(), counter.value);

    // FIXME: Take style containment into account
    // https://drafts.csswg.org/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 1. The 'counter-increment' and 'counter-set' properties must be scoped to the element’s sub-tree and create a
    //    new counter.

    // 3. Counter values are incremented (counter-increment).
    auto counter_increment = style.counter_data(CSS::PropertyID::CounterIncrement);
    for (auto const& counter : counter_increment)
        ensure_counters_set().increment_a_counter(counter.name, *this, *counter.value);

    // 4. Counter values are explicitly set (counter-set).
    auto counter_set = style.counter_data(CSS::PropertyID::CounterSet);
    for (auto const& counter : counter_set)
        ensure_counters_set().set_a_counter(counter.name, *this, *counter.value);

    // Ad-hoc: maybe update initial value for incremented reversed counters.
    for (auto const& counter : counter_increment) {
        auto existing_counter = ensure_counters_set().last_counter_with_name(counter.name);
        if (!existing_counter->value.has_value())
            continue;

        if (!existing_counter->reversed)
            continue;

        if (existing_counter->is_explicitly_set_reversed_counter)
            continue; // Counters which are explicitly set do not need to update the initial counter value if incremented.

        existing_counter->originating_element.update_initial_value_for_reversed_counter__after_increment(counter.name, counter.value.value().value());
    }

    // 5. Counter values are used (counter()/counters()).
    // NOTE: This happens when we process the `content` property.
}

// https://drafts.csswg.org/css-lists-3/#inherit-counters
void AbstractElement::inherit_counters()
{
    // 1. If element is the root of its document tree, the element has an initially-empty CSS counters set.
    //    Return.
    auto parent = parent_element();
    if (parent == nullptr) {
        // NOTE: We represent an empty counters set with `m_counters_set = nullptr`.
        set_counters_set(nullptr);
        return;
    }

    // 2. Let element counters, representing element’s own CSS counters set, be a copy of the CSS counters
    //    set of element’s parent element.
    OwnPtr<CSS::CountersSet> element_counters;
    // OPTIMIZATION: If parent has a set, we create a copy. Otherwise, we avoid allocating one until we need
    // to add something to it.
    auto ensure_element_counters = [&]() {
        if (!element_counters)
            element_counters = make<CSS::CountersSet>();
    };
    if (parent->has_non_empty_counters_set()) {
        element_counters = make<CSS::CountersSet>();
        *element_counters = *parent->counters_set();
    }

    // 3. Let sibling counters be the CSS counters set of element’s preceding sibling (if it has one),
    //    or an empty CSS counters set otherwise.
    //    For each counter of sibling counters, if element counters does not already contain a counter with
    //    the same name, append a copy of counter to element counters.
    if (auto sibling = previous_sibling_in_tree_order(); sibling.has_value() && sibling->has_non_empty_counters_set()) {
        auto& sibling_counters = sibling->counters_set().release_value();
        ensure_element_counters();
        for (auto const& counter : sibling_counters.counters()) {
            if (!element_counters->last_counter_with_name(counter.name).has_value())
                element_counters->append_copy(counter);
        }
    }

    // 4. Let value source be the CSS counters set of the element immediately preceding element in tree order.
    //    For each source counter of value source, if element counters contains a counter with the same name
    //    and creator, then set the value of that counter to source counter’s value.
    if (auto const previous = previous_in_tree_order(); previous.has_value() && previous->has_non_empty_counters_set()) {
        // NOTE: If element_counters is empty (AKA null) then we can skip this since nothing will match.
        if (element_counters) {
            auto& value_source = previous->counters_set().release_value();
            for (auto const& source_counter : value_source.counters()) {
                auto maybe_existing_counter = element_counters->counter_with_same_name_and_creator(source_counter.name, source_counter.originating_element);
                if (maybe_existing_counter.has_value()) {
                    maybe_existing_counter->value = source_counter.value;
                    maybe_existing_counter->is_explicitly_set_reversed_counter = source_counter.is_explicitly_set_reversed_counter;
                }
            }
        }
    }

    VERIFY(!element_counters || !element_counters->is_empty());
    set_counters_set(move(element_counters));
}

String AbstractElement::debug_description() const
{
    if (m_pseudo_element.has_value()) {
        StringBuilder builder;
        builder.append(m_element->debug_description());
        builder.append("::"sv);
        builder.append(CSS::pseudo_element_name(*m_pseudo_element));
        return builder.to_string_without_validation();
    }
    return m_element->debug_description();
}

}
