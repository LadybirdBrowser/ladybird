/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/HashFunctions.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

class CrossProcessId {
public:
    u64 namespace_id { 0 };
    u64 local_id { 0 };

    bool operator==(CrossProcessId const&) const = default;
};

class CrossProcessIdAllocator {
public:
    u64 namespace_id { 0 };
    u64 next_local_id { 1 };

    CrossProcessId allocate()
    {
        VERIFY(namespace_id > 0);
        VERIFY(next_local_id > 0);
        return CrossProcessId {
            .namespace_id = namespace_id,
            .local_id = next_local_id++,
        };
    }
};

}

template<>
struct AK::Traits<Web::HTML::CrossProcessId> : public DefaultTraits<Web::HTML::CrossProcessId> {
    static unsigned hash(Web::HTML::CrossProcessId const& id)
    {
        return pair_int_hash(Traits<u64>::hash(id.namespace_id), Traits<u64>::hash(id.local_id));
    }
};

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::CrossProcessId const&);
template<>
WEB_API ErrorOr<Web::HTML::CrossProcessId> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::CrossProcessIdAllocator const&);
template<>
WEB_API ErrorOr<Web::HTML::CrossProcessIdAllocator> decode(Decoder&);

}

template<>
struct AK::Formatter<Web::HTML::CrossProcessId> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::HTML::CrossProcessId const& id)
    {
        return Formatter<FormatString>::format(builder, "{}:{}"sv, id.namespace_id, id.local_id);
    }
};
