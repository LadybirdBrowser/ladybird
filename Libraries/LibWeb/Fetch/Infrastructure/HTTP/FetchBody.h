/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibCore/ImmutableBytes.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace Web::Fetch::Infrastructure {

// A body's source and length, without its stream.
class WEB_API FetchBody {
public:
    FetchBody() = default;
    FetchBody(Optional<Core::ImmutableBytes> source, Optional<u64> length);

    [[nodiscard]] static FetchBody from_bytes(Core::ImmutableBytes bytes)
    {
        auto length = bytes.size();
        return FetchBody { move(bytes), length };
    }

    [[nodiscard]] Optional<Core::ImmutableBytes> const& source() const { return m_source; }
    [[nodiscard]] Optional<u64> const& length() const { return m_length; }

private:
    // https://fetch.spec.whatwg.org/#concept-body-source
    // A source (null, a byte sequence, a Blob object, or a FormData object), initially null.
    // NB: A Blob or FormData source is held as its bytes.
    Optional<Core::ImmutableBytes> m_source;

    // https://fetch.spec.whatwg.org/#concept-body-total-bytes
    // A length (null or an integer), initially null.
    Optional<u64> m_length;
};

// A response's body as fetch keeps it: its length and where its bytes are.
class WEB_API ResponseBody {
public:
    // The bytes are read through the stream of the response's body view.
    struct ReadThroughStream { };
    using Bytes = Variant<Core::ImmutableBytes, NonnullRefPtr<Engine::FetchByteChannel>, ReadThroughStream>;

    ResponseBody(Bytes, Optional<u64> length);

    [[nodiscard]] static ResponseBody from_bytes(Core::ImmutableBytes);
    [[nodiscard]] static ResponseBody from_fetch_body(FetchBody const&);

    [[nodiscard]] Bytes const& bytes() const { return m_bytes; }

    // https://fetch.spec.whatwg.org/#concept-body-source
    [[nodiscard]] Optional<Core::ImmutableBytes const&> source() const;

    // https://fetch.spec.whatwg.org/#concept-body-total-bytes
    [[nodiscard]] Optional<u64> const& length() const { return m_length; }

private:
    Bytes m_bytes;
    Optional<u64> m_length;
};

WEB_API ResponseBody byte_sequence_as_response_body(ReadonlyBytes);

}
