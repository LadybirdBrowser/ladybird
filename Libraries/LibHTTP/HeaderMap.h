/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibHTTP/Header.h>

namespace HTTP {

class HeaderMap {
public:
    HeaderMap() = default;
    ~HeaderMap() = default;

    HeaderMap(Vector<Header> headers)
        : m_headers(move(headers))
    {
        for (auto& header : m_headers)
            m_map.set(header.name, header.value);
    }

    HeaderMap(HeaderMap const&) = default;
    HeaderMap(HeaderMap&&) = default;

    HeaderMap& operator=(HeaderMap const&) = default;
    HeaderMap& operator=(HeaderMap&&) = default;

    void set(ByteString name, ByteString value)
    {
        m_map.set(name, value);
        m_headers.append({ move(name), move(value) });
    }

    [[nodiscard]] bool contains(ByteString const& name) const
    {
        return m_map.contains(name);
    }

    [[nodiscard]] Optional<ByteString> get(ByteString const& name) const
    {
        return m_map.get(name);
    }

    [[nodiscard]] Vector<Header> const& headers() const
    {
        return m_headers;
    }

private:
    HashMap<ByteString, ByteString, CaseInsensitiveStringTraits> m_map;
    Vector<Header> m_headers;
};

}

namespace IPC {

template<>
inline ErrorOr<void> encode(Encoder& encoder, HTTP::HeaderMap const& header_map)
{
    TRY(encoder.encode(header_map.headers()));
    return {};
}

template<>
inline ErrorOr<HTTP::HeaderMap> decode(Decoder& decoder)
{
    auto headers = TRY(decoder.decode<Vector<HTTP::Header>>());
    return HTTP::HeaderMap { move(headers) };
}

}
