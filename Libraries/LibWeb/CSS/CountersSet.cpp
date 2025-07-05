/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS {

void CountersSet::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto const& counter : m_counters)
        counter.originating_element.visit(visitor);
}

// https://drafts.csswg.org/css-lists-3/#instantiate-counter
Counter& CountersSet::instantiate_a_counter(FlyString const& name, DOM::AbstractElement const& element, bool reversed, Optional<CounterValue> value)
{
    // 1. Let counters be element’s CSS counters set.

    // 2. Let innermost counter be the last counter in counters with the name name.
    //    If innermost counter’s originating element is element or a previous sibling of element,
    //    remove innermost counter from counters.
    auto innermost_counter = last_counter_with_name(name);
    if (innermost_counter.has_value()) {
        auto& originating_element = innermost_counter->originating_element;

        if (originating_element == element
            || (originating_element.parent_element() == element.parent_element() && originating_element.is_before(element))) {

            m_counters.remove_first_matching([&innermost_counter](auto& it) {
                return it.name == innermost_counter->name
                    && it.originating_element == innermost_counter->originating_element;
            });
        }
    }

    // 3. Append a new counter to counters with name name, originating element element,
    //    reversed being reversed, and initial value value (if given)
    m_counters.append({
        .name = move(name),
        .originating_element = element,
        .reversed = reversed,
        .value = value,
    });

    return m_counters.last();
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-set
void CountersSet::set_a_counter(FlyString const& name, DOM::AbstractElement const& element, CounterValue value)
{
    auto existing_counter = last_counter_with_name(name);

    if (!existing_counter.has_value()) {
        // If there is not currently a counter of the given name on the element, the element instantiates
        // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
        // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
        auto& counter = instantiate_a_counter(name, element, false, 0);
        counter.value = value;
        return;
    }

    existing_counter->value = value;

    if (existing_counter->reversed && !existing_counter->is_explicitly_set_reversed_counter) {
        existing_counter->originating_element.update_initial_value_for_reversed_counter__after_set(name, value.value());
        existing_counter->is_explicitly_set_reversed_counter = true;
    }
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
void CountersSet::increment_a_counter(FlyString const& name, DOM::AbstractElement const& element, CounterValue amount)
{
    auto existing_counter = last_counter_with_name(name);

    if (!existing_counter.has_value()) {
        // If there is not currently a counter of the given name on the element, the element instantiates
        // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
        // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
        auto& counter = instantiate_a_counter(name, element, false, 0);
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

Optional<Counter&> CountersSet::counter_with_same_name_and_creator(FlyString const& name, DOM::AbstractElement const& element)
{
    return m_counters.first_matching([&](auto& it) {
        return it.name == name && it.originating_element == element;
    });
}

void CountersSet::append_copy(Counter const& counter)
{
    m_counters.append(counter);
}

String CountersSet::dump() const
{
    StringBuilder builder;
    builder.append("{\n"sv);
    for (auto const& counter : m_counters) {
        builder.appendff("    {} ({}) = {}\n", counter.name, counter.originating_element.debug_description(), counter.value);
    }
    builder.append('}');
    return builder.to_string_without_validation();
}

}
