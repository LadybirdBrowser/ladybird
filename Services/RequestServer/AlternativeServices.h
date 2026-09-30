/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <RequestServer/IsPrivate.h>

namespace RequestServer {

// HTTP/2 alternatives are unsupported: libcurl cannot require HTTP/2 negotiation.
struct AlternativeService {
    bool operator==(AlternativeService const&) const = default;

    // Empty when the alternative is on the origin's own host.
    ByteString host;
    u16 port { 0 };

    UnixDateTime expires_at;
    UnixDateTime received_at;
};

// Alternatives with other protocols are left out, so "clear" and a value listing no HTTP/3 alternative both parse to an
// empty list.
Optional<Vector<AlternativeService>> parse_alt_svc(StringView, UnixDateTime now, AK::Duration age = {});

class AlternativeServiceCache {
public:
    static AlternativeServiceCache& the();

    static constexpr size_t maximum_alternatives_per_origin = 8;
    static constexpr size_t maximum_origins = 1024;
    static constexpr AK::Duration broken_alternative_timeout = AK::Duration::from_seconds(300);

    void update(IsPrivate, URL::URL const& origin, StringView alt_svc, UnixDateTime now, AK::Duration age = {});

    Optional<AlternativeService> find(IsPrivate, URL::URL const& origin, UnixDateTime now);

    // Suppress failed alternatives even if the origin advertises them again.
    void mark_broken(IsPrivate, URL::URL const& origin, AlternativeService const&, UnixDateTime now);

    void remove_entries_received_since(UnixDateTime);
    void clear(IsPrivate);

private:
    static ByteString key_for(IsPrivate, URL::URL const& origin);
    static ByteString broken_key_for(ByteString const& origin_key, AlternativeService const&);

    HashMap<ByteString, Vector<AlternativeService>> m_alternatives;
    HashMap<ByteString, UnixDateTime> m_broken_until;
};

}
