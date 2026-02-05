/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stddef.h>

namespace Line {

class Span {
public:
    enum Mode {
        ByteOriented,
        CodepointOriented,
    };

    Span(size_t start, size_t end, Mode mode = ByteOriented)
        : m_beginning(start)
        , m_end(end)
        , m_mode(mode)
    {
    }

    size_t beginning() const { return m_beginning; }
    size_t end() const { return m_end; }
    Mode mode() const { return m_mode; }

    bool is_empty() const
    {
        return m_beginning < m_end;
    }

private:
    size_t m_beginning { 0 };
    size_t m_end { 0 };
    Mode m_mode { CodepointOriented };
};

}
