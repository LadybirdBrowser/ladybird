/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Optional.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace Web::HTML {

struct ScriptRegistryContent {
    Utf16String content_type;
    Utf16String text;
};

struct ScriptRegistryIdentifier {
    UniqueNodeID document_id;
    u64 script_id { 0 };

    bool operator==(ScriptRegistryIdentifier const&) const = default;
};

struct ScriptRegistryDescription {
    ScriptRegistryIdentifier id;
    Optional<URL::URL> url;
    Utf16String display_url;
    Utf16String introduction_type;
    Utf16String content_type;
    bool is_inline_source { false };
    u32 source_start_line { 1 };
    u32 source_start_column { 0 };
    size_t source_length { 0 };
};

}

template<>
struct AK::Traits<Web::HTML::ScriptRegistryIdentifier> : public AK::DefaultTraits<Web::HTML::ScriptRegistryIdentifier> {
    static bool equals(Web::HTML::ScriptRegistryIdentifier const& lhs, Web::HTML::ScriptRegistryIdentifier const& rhs)
    {
        return lhs == rhs;
    }

    static unsigned hash(Web::HTML::ScriptRegistryIdentifier const& identifier)
    {
        return pair_int_hash(identifier.document_id.value(), identifier.script_id);
    }
};

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::ScriptRegistryIdentifier const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::ScriptRegistryIdentifier> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::ScriptRegistryDescription const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::ScriptRegistryDescription> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::ScriptRegistryContent const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::ScriptRegistryContent> decode(Decoder&);

}
