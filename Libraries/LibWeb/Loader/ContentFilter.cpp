/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/Span.h>
#include <LibWeb/Loader/ContentFilter.h>

namespace Web {

ContentFilter& ContentFilter::the()
{
    static ContentFilter filter;
    return filter;
}

ContentFilter::ContentFilter() = default;

ContentFilter::~ContentFilter() = default;

bool ContentFilter::is_filtered(URL::URL const& url) const
{
    if (!filtering_enabled())
        return false;

    if (url.scheme() == "data")
        return false;
    return contains(url.to_string());
}

bool ContentFilter::contains(StringView text) const
{
    if (!m_matcher)
        return false;
    return m_matcher->contains(text);
}

ErrorOr<void> ContentFilter::set_patterns(ReadonlySpan<String> patterns)
{
    m_matcher = make<AsciiStringMatcher>(patterns);
    return {};
}

AsciiStringMatcher::AsciiStringMatcher(ReadonlySpan<String> patterns)
{
    struct BuildTimeNode {
        Vector<Transition> children;
        bool is_output { false };
    };

    Vector<BuildTimeNode> build_time_nodes;
    build_time_nodes.append({});

    for (u32 i = 0; i < patterns.size(); ++i) {
        auto const& pattern = patterns[i];
        u32 node = 0;
        for (u8 ch : pattern.bytes_as_string_view()) {
            VERIFY(is_ascii(ch));
            auto it = build_time_nodes[node].children.find_if(
                [ch](Transition const& t) { return t.character == ch; });

            if (it != build_time_nodes[node].children.end()) {
                node = it->next_state;
            } else {
                u32 new_node = build_time_nodes.size();
                build_time_nodes.append({});
                build_time_nodes[node].children.empend(ch, new_node);
                node = new_node;
            }
        }

        if (!build_time_nodes[node].is_output)
            build_time_nodes[node].is_output = true;
    }

    Vector<u32> failure_links;
    failure_links.resize(build_time_nodes.size());

    Queue<u32> queue;
    for (auto const& transition : build_time_nodes[0].children) {
        u32 child = transition.next_state;
        failure_links[child] = 0;
        queue.enqueue(child);
    }

    while (!queue.is_empty()) {
        u32 current = queue.dequeue();
        for (auto& [character, child] : build_time_nodes[current].children) {
            u32 failure_link = failure_links[current];
            while (failure_link != 0) {
                auto it = build_time_nodes[failure_link].children.find_if(
                    [character](Transition const& tr) { return tr.character == character; });
                if (it != build_time_nodes[failure_link].children.end()) {
                    failure_link = it->next_state;
                    break;
                }
                failure_link = failure_links[failure_link];
            }

            u32 next_failure_link = failure_link;
            failure_links[child] = next_failure_link;

            bool inherited = build_time_nodes[next_failure_link].is_output;
            if (inherited && !build_time_nodes[child].is_output)
                build_time_nodes[child].is_output = true;

            queue.enqueue(child);
        }
    }

    for (auto& node : build_time_nodes) {
        quick_sort(node.children, [](Transition const& a, Transition const& b) {
            return a.character < b.character;
        });
    }

    m_nodes.resize(build_time_nodes.size());
    m_transitions.clear_with_capacity();

    u32 transition_index = 0;
    for (u32 i = 0; i < build_time_nodes.size(); ++i) {
        auto& build_time_node = build_time_nodes[i];
        m_nodes[i].first_transition = transition_index;
        m_nodes[i].transition_count = build_time_node.children.size();
        m_nodes[i].output = build_time_node.is_output;
        m_transitions.extend(build_time_node.children);

        transition_index += build_time_node.children.size();
    }
}

bool AsciiStringMatcher::contains(StringView text) const
{
    if (m_nodes.is_empty())
        return false;

    auto get_children = [this](u32 state) -> ReadonlySpan<Transition> {
        return m_transitions.span().slice(m_nodes[state].first_transition, m_nodes[state].transition_count);
    };

    u32 state = 0;
    for (u8 ch : text.bytes()) {
        auto const& children = get_children(state);

        auto const* found = AK::binary_search(
            children,
            ch,
            nullptr,
            [](u8 needle, Transition const& transition) {
                if (needle > transition.character)
                    return needle < transition.character ? -1 : 1;
                return needle < transition.character ? -1 : 0;
            });

        if (!found) {
            state = 0;
            auto const& root_children = get_children(0);
            found = AK::binary_search(
                root_children,
                ch,
                nullptr,
                [](u8 needle, Transition const& transition) {
                    if (needle > transition.character)
                        return needle < transition.character ? -1 : 1;
                    return needle < transition.character ? -1 : 0;
                });
            if (!found)
                continue;
        }

        state = found->next_state;
        if (m_nodes[state].output)
            return true;
    }
    return false;
}

}
