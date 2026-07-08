/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// "UAs may have implementation-specific limits on the maximum or minimum value of a counter.
// If a counter reset, set, or increment would push the value outside of that range, the value
// must be clamped to that range." - https://drafts.csswg.org/css-lists-3/#auto-numbering
using CounterValue = i32;

// https://drafts.csswg.org/css-lists-3/#counter
struct Counter {
    Utf16FlyString name;
    DOM::AbstractElement originating_element; // "creator"
    bool reversed { false };
    Optional<CounterValue> value;
};

// https://drafts.csswg.org/css-lists-3/#css-counters-set
class CountersSet {
public:
    CountersSet() = default;
    ~CountersSet() = default;

    Counter& instantiate_a_counter(Utf16FlyString name, DOM::AbstractElement const&, bool reversed, Optional<CounterValue>);
    void set_a_counter(Utf16FlyString name, DOM::AbstractElement const&, CounterValue value);
    void increment_a_counter(Utf16FlyString name, DOM::AbstractElement const&, CounterValue amount);
    void append_copy(Counter const&);

    Optional<Counter&> last_counter_with_name(Utf16FlyString const& name);
    Optional<Counter&> counter_with_same_name_and_creator(Utf16FlyString const& name, DOM::AbstractElement const&);

    Vector<Counter> const& counters() const { return m_counters; }
    bool is_empty() const { return m_counters.is_empty(); }

    void visit_edges(GC::Cell::Visitor&);

    String dump() const;

private:
    Vector<Counter> m_counters;
};

void resolve_counters(DOM::AbstractElement&);
void inherit_counters(DOM::AbstractElement&);

}
