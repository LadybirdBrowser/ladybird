/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyle.h"
#include <AK/HashTable.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

Optional<String> CounterStyle::generate_an_initial_representation_for_the_counter_value(i32) const
{
    return m_algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const&) -> Optional<String> {
            TODO();
        },
        [&](FixedCounterStyleAlgorithm const&) -> Optional<String> {
            TODO();
        },
        [&](GenericCounterStyleAlgorithm const& generic_algorithm) -> Optional<String> {
            switch (generic_algorithm.type) {
            case CounterStyleSystem::Cyclic:
            case CounterStyleSystem::Numeric:
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Symbolic:
                TODO();
            case CounterStyleSystem::Additive:
                // NB: This is handled by AdditiveCounterStyleAlgorithm.
                VERIFY_NOT_REACHED();
            }

            VERIFY_NOT_REACHED();
        });
}

}
