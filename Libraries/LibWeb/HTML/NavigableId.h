/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/HashFunctions.h>
#include <AK/Optional.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

struct NavigableId {
    u64 namespace_id { 0 };
    u64 local_id { 0 };

    bool operator==(NavigableId const&) const = default;
};

struct NavigableIdAllocator {
    u64 namespace_id { 0 };
    u64 next_local_id { 1 };

    NavigableId allocate()
    {
        VERIFY(namespace_id > 0);
        VERIFY(next_local_id > 0);
        return NavigableId {
            .namespace_id = namespace_id,
            .local_id = next_local_id++,
        };
    }
};

}

template<>
struct AK::Traits<Web::HTML::NavigableId> : public DefaultTraits<Web::HTML::NavigableId> {
    static unsigned hash(Web::HTML::NavigableId const& id)
    {
        return pair_int_hash(Traits<u64>::hash(id.namespace_id), Traits<u64>::hash(id.local_id));
    }
};

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigableId const&);
template<>
WEB_API ErrorOr<Web::HTML::NavigableId> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigableIdAllocator const&);
template<>
WEB_API ErrorOr<Web::HTML::NavigableIdAllocator> decode(Decoder&);

}

template<>
struct AK::Formatter<Web::HTML::NavigableId> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::HTML::NavigableId const& id)
    {
        return Formatter<FormatString>::format(builder, "{}:{}"sv, id.namespace_id, id.local_id);
    }
};
