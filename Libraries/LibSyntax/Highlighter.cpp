/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibSyntax/Highlighter.h>

namespace Syntax {

void Highlighter::highlight_matching_token_pair()
{
    auto& document = m_client->get_document();

    enum class Direction {
        Forward,
        Backward,
    };

    auto find_span_of_type = [&](auto i, u64 type, u64 not_type, Direction direction) -> Optional<size_t> {
        size_t nesting_level = 0;
        bool forward = direction == Direction::Forward;

        if (forward) {
            ++i;
            if (i >= document.spans().size())
                return {};
        } else {
            if (i == 0)
                return {};
            --i;
        }

        for (;;) {
            auto& span = document.spans().at(i);
            auto span_token_type = span.data;
            if (token_types_equal(span_token_type, not_type)) {
                ++nesting_level;
            } else if (token_types_equal(span_token_type, type)) {
                if (nesting_level-- <= 0)
                    return i;
            }

            if (forward) {
                ++i;
                if (i >= document.spans().size())
                    return {};
            } else {
                if (i == 0)
                    return {};
                --i;
            }
        }

        return {};
    };

    auto make_buddies = [&](int index0, int index1) {
        auto& buddy0 = document.spans()[index0];
        auto& buddy1 = document.spans()[index1];
        m_has_brace_buddies = true;
        m_brace_buddies[0].index = index0;
        m_brace_buddies[1].index = index1;
        m_brace_buddies[0].span_backup = buddy0;
        m_brace_buddies[1].span_backup = buddy1;
        buddy0.attributes.background_color = Color::DarkCyan;
        buddy1.attributes.background_color = Color::DarkCyan;
        buddy0.attributes.color = Color::White;
        buddy1.attributes.color = Color::White;
        m_client->do_update();
    };

    auto pairs = matching_token_pairs();

    for (size_t i = 0; i < document.spans().size(); ++i) {
        auto& span = const_cast<TextDocumentSpan&>(document.spans().at(i));
        auto token_type = span.data;

        for (auto& pair : pairs) {
            if (token_types_equal(token_type, pair.open) && span.range.start() == m_client->get_cursor()) {
                auto buddy = find_span_of_type(i, pair.close, pair.open, Direction::Forward);
                if (buddy.has_value())
                    make_buddies(i, buddy.value());
                return;
            }
        }

        for (auto& pair : pairs) {
            if (token_types_equal(token_type, pair.close) && span.range.end() == m_client->get_cursor()) {
                auto buddy = find_span_of_type(i, pair.open, pair.close, Direction::Backward);
                if (buddy.has_value())
                    make_buddies(i, buddy.value());
                return;
            }
        }
    }
}

void Highlighter::attach(HighlighterClient& client)
{
    VERIFY(!m_client);
    m_client = &client;
}

void Highlighter::detach()
{
    m_client = nullptr;
}

void Highlighter::cursor_did_change()
{
    auto& document = m_client->get_document();
    if (m_has_brace_buddies) {
        if (m_brace_buddies[0].index >= 0 && m_brace_buddies[0].index < static_cast<int>(document.spans().size()))
            document.set_span_at_index(m_brace_buddies[0].index, m_brace_buddies[0].span_backup);
        if (m_brace_buddies[1].index >= 0 && m_brace_buddies[1].index < static_cast<int>(document.spans().size()))
            document.set_span_at_index(m_brace_buddies[1].index, m_brace_buddies[1].span_backup);
        m_has_brace_buddies = false;
        m_client->do_update();
    }
    highlight_matching_token_pair();
}

Vector<Highlighter::MatchingTokenPair> Highlighter::matching_token_pairs() const
{
    auto own_pairs = matching_token_pairs_impl();
    own_pairs.ensure_capacity(own_pairs.size() + m_nested_token_pairs.size());
    for (auto& nested_pair : m_nested_token_pairs)
        own_pairs.append(nested_pair);
    return own_pairs;
}

void Highlighter::register_nested_token_pairs(Vector<MatchingTokenPair> pairs)
{
    for (auto& pair : pairs)
        m_nested_token_pairs.set(pair);
}

}
