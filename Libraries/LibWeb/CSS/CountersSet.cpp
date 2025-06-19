/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CountersSet.h"
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-lists-3/#instantiate-counter
Counter& CountersSet::instantiate_a_counter(FlyString const& name, UniqueNodeID originating_element_id, bool reversed, Optional<CounterValue> value)
{
    // 1. Let counters be element’s CSS counters set.
    auto* element = DOM::Node::from_unique_id(originating_element_id);

    // 2. Let innermost counter be the last counter in counters with the name name.
    //    If innermost counter’s originating element is element or a previous sibling of element,
    //    remove innermost counter from counters.
    auto innermost_counter = last_counter_with_name(name);
    if (innermost_counter.has_value()) {
        auto* originating_node = DOM::Node::from_unique_id(innermost_counter->originating_element_id);
        VERIFY(originating_node);
        auto& originating_element = as<DOM::Element>(*originating_node);

        if (&originating_element == element
            || (originating_element.parent() == element->parent() && originating_element.is_before(*element))) {

            m_counters.remove_first_matching([&innermost_counter](auto& it) {
                return it.name == innermost_counter->name
                    && it.originating_element_id == innermost_counter->originating_element_id;
            });
        }
    }

    // 3. Append a new counter to counters with name name, originating element element,
    //    reversed being reversed, and initial value value (if given)
    m_counters.append({
        .name = move(name),
        .originating_element_id = originating_element_id,
        .reversed = reversed,
        .value = value,
    });

    return m_counters.last();
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-set
void CountersSet::set_a_counter(FlyString const& name, UniqueNodeID originating_element_id, CounterValue value)
{
    auto existing_counter = last_counter_with_name(name);

    if (!existing_counter.has_value()) {
        // If there is not currently a counter of the given name on the element, the element instantiates
        // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
        // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
        auto& counter = instantiate_a_counter(name, originating_element_id, false, 0);
        VERIFY(!counter.reversed); // Reversed counters cannot be instantiated by a counter-set instruction.
        counter.value = value;
        return;
    }

    existing_counter->value = value;

    if (existing_counter->reversed && !existing_counter->is_explicitly_set_reversed_counter) {
        auto* originating_node = DOM::Node::from_unique_id(existing_counter->originating_element_id);
        VERIFY(originating_node->is_element());
        auto* originating_element = static_cast<DOM::Element*>(originating_node);

        originating_element->update_initial_value_for_reversed_counter__after_set(name, value.value());
        existing_counter->is_explicitly_set_reversed_counter = true;
    }
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
void CountersSet::increment_a_counter(FlyString const& name, UniqueNodeID originating_element_id, CounterValue amount)
{
    auto existing_counter = last_counter_with_name(name);

    if (!existing_counter.has_value()) {
        // If there is not currently a counter of the given name on the element, the element instantiates
        // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
        // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
        auto& counter = instantiate_a_counter(name, originating_element_id, false, 0);
        VERIFY(!counter.reversed); // Reversed counters cannot be instantiated by a counter-increment instruction.
        counter.value->saturating_add(amount.value());
        return;
    }

    if (!existing_counter->value.has_value())
        existing_counter->value = 0;

    existing_counter->value->saturating_add(amount.value());
}

Optional<Counter&> CountersSet::last_counter_with_name(FlyString const& name)
{
    for (auto& counter : m_counters.in_reverse()) {
        if (counter.name == name)
            return counter;
    }
    return {};
}

Optional<Counter&> CountersSet::counter_with_same_name_and_creator(FlyString const& name, UniqueNodeID originating_element_id)
{
    return m_counters.first_matching([&](auto& it) {
        return it.name == name && it.originating_element_id == originating_element_id;
    });
}

void CountersSet::append_copy(Counter const& counter)
{
    m_counters.append(counter);
}

}
