/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace Web::CSS {

struct StyleSheetIdentifier {
    enum class Type : u8 {
        StyleElement,
        LinkElement,
        ImportRule,
        UserAgent,
        UserStyle,
    };

    Type type;
    Optional<UniqueNodeID> dom_element_unique_id {};
    Optional<Utf16String> url {};
    size_t rule_count { 0 };

    bool operator==(StyleSheetIdentifier const& other) const
    {
        return type == other.type && dom_element_unique_id == other.dom_element_unique_id && url == other.url;
    }
};

WEBCOMMON_API StringView style_sheet_identifier_type_to_string(StyleSheetIdentifier::Type);
WEBCOMMON_API Optional<StyleSheetIdentifier::Type> style_sheet_identifier_type_from_string(StringView);

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::CSS::StyleSheetIdentifier const&);

template<>
WEBCOMMON_API ErrorOr<Web::CSS::StyleSheetIdentifier> decode(Decoder&);

}
