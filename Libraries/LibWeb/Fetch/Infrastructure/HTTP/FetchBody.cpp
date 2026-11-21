/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/FetchBody.h>

namespace Web::Fetch::Infrastructure {

FetchBody::FetchBody(Optional<Core::ImmutableBytes> source, Optional<u64> length)
    : m_source(move(source))
    , m_length(length)
{
}

ResponseBody::ResponseBody(Bytes bytes, Optional<u64> length)
    : m_bytes(move(bytes))
    , m_length(length)
{
}

ResponseBody ResponseBody::from_bytes(Core::ImmutableBytes bytes)
{
    auto length = bytes.size();
    return ResponseBody { move(bytes), length };
}

ResponseBody ResponseBody::from_fetch_body(FetchBody const& body)
{
    if (auto const& source = body.source(); source.has_value())
        return ResponseBody { *source, body.length() };
    return ResponseBody { ReadThroughStream {}, body.length() };
}

Optional<Core::ImmutableBytes const&> ResponseBody::source() const
{
    if (auto const* bytes = m_bytes.get_pointer<Core::ImmutableBytes>())
        return *bytes;
    return {};
}

// https://fetch.spec.whatwg.org/#byte-sequence-as-a-body
// NB: Each view of the body makes its stream from these bytes.
ResponseBody byte_sequence_as_response_body(ReadonlyBytes bytes)
{
    return ResponseBody::from_bytes(MUST(Core::ImmutableBytes::copy(bytes)));
}

}
