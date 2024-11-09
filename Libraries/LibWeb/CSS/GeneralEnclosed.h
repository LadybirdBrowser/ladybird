/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::CSS {

// Corresponds to Kleene 3-valued logic.
// https://www.w3.org/TR/mediaqueries-4/#evaluating
enum class MatchResult {
    False,
    True,
    Unknown,
};

inline MatchResult as_match_result(bool value)
{
    return value ? MatchResult::True : MatchResult::False;
}

inline MatchResult negate(MatchResult value)
{
    switch (value) {
    case MatchResult::False:
        return MatchResult::True;
    case MatchResult::True:
        return MatchResult::False;
    case MatchResult::Unknown:
        return MatchResult::Unknown;
    }
    VERIFY_NOT_REACHED();
}

template<typename Collection, typename Evaluate>
inline MatchResult evaluate_and(Collection& collection, Evaluate evaluate)
{
    size_t true_results = 0;
    for (auto& item : collection) {
        auto item_match = evaluate(item);
        if (item_match == MatchResult::False)
            return MatchResult::False;
        if (item_match == MatchResult::True)
            true_results++;
    }
    if (true_results == collection.size())
        return MatchResult::True;
    return MatchResult::Unknown;
}

template<typename Collection, typename Evaluate>
inline MatchResult evaluate_or(Collection& collection, Evaluate evaluate)
{
    size_t false_results = 0;
    for (auto& item : collection) {
        auto item_match = evaluate(item);
        if (item_match == MatchResult::True)
            return MatchResult::True;
        if (item_match == MatchResult::False)
            false_results++;
    }
    if (false_results == collection.size())
        return MatchResult::False;
    return MatchResult::Unknown;
}

// https://www.w3.org/TR/mediaqueries-4/#typedef-general-enclosed
class GeneralEnclosed {
public:
    GeneralEnclosed(String serialized_contents)
        : m_serialized_contents(move(serialized_contents))
    {
    }

    MatchResult evaluate() const { return MatchResult::Unknown; }
    String const& to_string() const { return m_serialized_contents; }

private:
    String m_serialized_contents;
};
}
