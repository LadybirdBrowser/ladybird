/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web {

class AsciiStringMatcher {
public:
    explicit AsciiStringMatcher(ReadonlySpan<String> patterns);

    bool contains(StringView text) const;

private:
    struct Transition {
        u8 character { 0 };
        u32 next_state { 0 };
    };

    struct Node {
        u32 first_transition { 0 };
        u8 transition_count { 0 };
        bool output { false };
    };

    Vector<Node> m_nodes;
    Vector<Transition> m_transitions;
};

class WEB_API ContentFilter {
public:
    static ContentFilter& the();

    bool filtering_enabled() const { return m_filtering_enabled; }
    void set_filtering_enabled(bool const enabled) { m_filtering_enabled = enabled; }

    bool is_filtered(URL::URL const&) const;
    ErrorOr<void> set_patterns(ReadonlySpan<String>);

private:
    ContentFilter();
    ~ContentFilter();

    bool contains(StringView text) const;

    bool m_filtering_enabled { true };
    OwnPtr<AsciiStringMatcher> m_matcher;
};

}
