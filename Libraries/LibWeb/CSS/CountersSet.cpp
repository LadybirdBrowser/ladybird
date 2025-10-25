/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
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
Counter& CountersSet::instantiate_a_counter(FlyString name, DOM::AbstractElement const& element, bool reversed, Optional<CounterValue> value)
{
    // 1. Let counters be element’s CSS counters set.

    // 2. Let innermost counter be the last counter in counters with the name name.
    //    If innermost counter’s originating element is element or a previous sibling of element,
    //    remove innermost counter from counters.
    auto innermost_counter = last_counter_with_name(name);
    if (innermost_counter.has_value()) {
        auto& innermost_element = innermost_counter->originating_element;

        if (innermost_element == element
            || (innermost_element.parent_element() == element.parent_element() && innermost_element.is_before(element))) {

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
    if (auto existing_counter = last_counter_with_name(name); existing_counter.has_value()) {
        existing_counter->value = value;
        return;
    }

    // If there is not currently a counter of the given name on the element, the element instantiates
    // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
    // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
    auto& counter = instantiate_a_counter(name, element, false, 0);
    counter.value = value;
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
void CountersSet::increment_a_counter(FlyString const& name, DOM::AbstractElement const& element, CounterValue amount)
{
    if (auto existing_counter = last_counter_with_name(name); existing_counter.has_value()) {
        // FIXME: How should we handle existing counters with no value? Can that happen?
        VERIFY(existing_counter->value.has_value());
        existing_counter->value->saturating_add(amount.value());
        return;
    }

    // If there is not currently a counter of the given name on the element, the element instantiates
    // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
    // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
    auto& counter = instantiate_a_counter(name, element, false, 0);
    counter.value->saturating_add(amount.value());
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

// https://drafts.csswg.org/css-lists-3/#auto-numbering
void resolve_counters(DOM::AbstractElement& element_reference)
{
    // Resolving counter values on a given element is a multi-step process:
    auto const& style = *element_reference.computed_properties();

    // 1. Existing counters are inherited from previous elements.
    inherit_counters(element_reference);

    // https://drafts.csswg.org/css-lists-3/#counters-without-boxes
    // An element that does not generate a box (for example, an element with display set to none,
    // or a pseudo-element with content set to none) cannot set, reset, or increment a counter.
    // The counter properties are still valid on such an element, but they must have no effect.
    if (style.display().is_none())
        return;

    // 2. New counters are instantiated (counter-reset).
    auto counter_reset = style.counter_data(PropertyID::CounterReset);
    for (auto const& counter : counter_reset)
        element_reference.ensure_counters_set().instantiate_a_counter(counter.name, element_reference, counter.is_reversed, counter.value);

    // FIXME: Take style containment into account
    // https://drafts.csswg.org/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 1. The 'counter-increment' and 'counter-set' properties must be scoped to the element’s sub-tree and create a
    //    new counter.

    // 3. Counter values are incremented (counter-increment).
    auto counter_increment = style.counter_data(PropertyID::CounterIncrement);
    for (auto const& counter : counter_increment)
        element_reference.ensure_counters_set().increment_a_counter(counter.name, element_reference, *counter.value);

    // 4. Counter values are explicitly set (counter-set).
    auto counter_set = style.counter_data(PropertyID::CounterSet);
    for (auto const& counter : counter_set)
        element_reference.ensure_counters_set().set_a_counter(counter.name, element_reference, *counter.value);

    // 5. Counter values are used (counter()/counters()).
    // NOTE: This happens when we process the `content` property.
}

// https://drafts.csswg.org/css-lists-3/#inherit-counters
void inherit_counters(DOM::AbstractElement& element_reference)
{
    // 1. If element is the root of its document tree, the element has an initially-empty CSS counters set.
    //    Return.
    auto parent = element_reference.parent_element();
    if (parent == nullptr) {
        // NOTE: We represent an empty counters set with `m_counters_set = nullptr`.
        element_reference.set_counters_set(nullptr);
        return;
    }

    // 2. Let element counters, representing element’s own CSS counters set, be a copy of the CSS counters
    //    set of element’s parent element.
    OwnPtr<CountersSet> element_counters;
    // OPTIMIZATION: If parent has a set, we create a copy. Otherwise, we avoid allocating one until we need
    // to add something to it.
    auto ensure_element_counters = [&]() {
        if (!element_counters)
            element_counters = make<CountersSet>();
    };
    if (parent->has_non_empty_counters_set()) {
        element_counters = make<CountersSet>();
        *element_counters = *parent->counters_set();
    }

    // 3. Let sibling counters be the CSS counters set of element’s preceding sibling (if it has one),
    //    or an empty CSS counters set otherwise.
    //    For each counter of sibling counters, if element counters does not already contain a counter with
    //    the same name, append a copy of counter to element counters.
    if (auto sibling = element_reference.previous_sibling_in_tree_order(); sibling.has_value() && sibling->has_non_empty_counters_set()) {
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
    if (auto const previous = element_reference.previous_in_tree_order(); previous.has_value() && previous->has_non_empty_counters_set()) {
        // NOTE: If element_counters is empty (AKA null) then we can skip this since nothing will match.
        if (element_counters) {
            auto& value_source = previous->counters_set().release_value();
            for (auto const& source_counter : value_source.counters()) {
                auto maybe_existing_counter = element_counters->counter_with_same_name_and_creator(source_counter.name, source_counter.originating_element);
                if (maybe_existing_counter.has_value())
                    maybe_existing_counter->value = source_counter.value;
            }
        }
    }

    VERIFY(!element_counters || !element_counters->is_empty());
    element_reference.set_counters_set(move(element_counters));
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
