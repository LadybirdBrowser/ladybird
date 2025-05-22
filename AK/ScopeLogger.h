/*
 * Copyright (c) 2020, Denis Campredon <deni_@hotmail.fr>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/SourceLocation.h>
#include <AK/StringBuilder.h>

namespace AK {

template<bool = true>
class ScopeLogger {
public:
    ScopeLogger(StringView extra, SourceLocation const& location = SourceLocation::current())
        : m_location(location)
        , m_extra(extra)
    {
        StringBuilder sb;

        for (auto indent = m_depth++; indent > 0; indent--)
            sb.append(' ');
        if (m_extra.is_empty())
            dbgln("\033[1;{}m{}entering {}\033[0m", m_depth % 8 + 30, sb.to_byte_string(), m_location);
        else
            dbgln("\033[1;{}m{}entering {}\033[0m ({})", m_depth % 8 + 30, sb.to_byte_string(), m_location, m_extra);
    }

    ScopeLogger(SourceLocation location = SourceLocation::current())
        : ScopeLogger({}, move(location))
    {
    }

    ~ScopeLogger()
    {
        StringBuilder sb;

        auto depth = m_depth;
        for (auto indent = --m_depth; indent > 0; indent--)
            sb.append(' ');
        if (m_extra.is_empty())
            dbgln("\033[1;{}m{}leaving {}\033[0m", depth % 8 + 30, sb.to_byte_string(), m_location);
        else
            dbgln("\033[1;{}m{}leaving {}\033[0m ({})", depth % 8 + 30, sb.to_byte_string(), m_location, m_extra);
    }

private:
    static inline size_t m_depth = 0;
    SourceLocation m_location;
    ByteString m_extra;
};

template<>
class ScopeLogger<false> {
public:
    template<typename... Args>
    ScopeLogger(Args...) { }
};

}

#if USING_AK_GLOBALLY
using AK::ScopeLogger;
#endif
