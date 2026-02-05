/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibSyntax/TextPosition.h>

namespace Syntax {

class TextRange {
public:
    TextRange() = default;
    TextRange(TextPosition const& start, TextPosition const& end)
        : m_start(start)
        , m_end(end)
    {
    }

    bool is_valid() const { return m_start.is_valid() && m_end.is_valid() && m_start != m_end; }
    void clear()
    {
        m_start = {};
        m_end = {};
    }

    TextPosition& start() { return m_start; }
    TextPosition& end() { return m_end; }
    TextPosition const& start() const { return m_start; }
    TextPosition const& end() const { return m_end; }

    size_t line_count() const { return normalized_end().line() - normalized_start().line() + 1; }

    TextRange normalized() const { return TextRange(normalized_start(), normalized_end()); }

    void set_start(TextPosition const& position) { m_start = position; }
    void set_end(TextPosition const& position) { m_end = position; }

    void set(TextPosition const& start, TextPosition const& end)
    {
        m_start = start;
        m_end = end;
    }

    bool operator==(TextRange const& other) const
    {
        return m_start == other.m_start && m_end == other.m_end;
    }

    bool contains(TextPosition const& position) const
    {
        if (position.line() <= m_start.line() && (position.line() != m_start.line() || position.column() < m_start.column()))
            return false;
        if (position.line() >= m_end.line() && (position.line() != m_end.line() || position.column() > m_end.column()))
            return false;
        return true;
    }

private:
    TextPosition normalized_start() const { return m_start < m_end ? m_start : m_end; }
    TextPosition normalized_end() const { return m_start < m_end ? m_end : m_start; }

    TextPosition m_start {};
    TextPosition m_end {};
};

}

template<>
struct AK::Formatter<Syntax::TextRange> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Syntax::TextRange const& value)
    {
        if (value.is_valid())
            return Formatter<FormatString>::format(builder, "{}-{}"sv, value.start(), value.end());
        return builder.put_string("TextRange(Invalid)"sv);
    }
};
