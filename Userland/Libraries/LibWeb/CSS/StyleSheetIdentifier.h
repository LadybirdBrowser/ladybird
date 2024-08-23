/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibURL/URL.h>

namespace Web::CSS {

struct StyleSheetIdentifier {
    enum class Type : u8 {
        StyleElement,
        LinkElement,
        ImportRule,
        UserAgent,
        UserStyle,
    } type;
    Optional<i32> dom_element_unique_id {};
    Optional<String> url {};
};

StringView style_sheet_identifier_type_to_string(StyleSheetIdentifier::Type);
Optional<StyleSheetIdentifier::Type> style_sheet_identifier_type_from_string(StringView);
}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::CSS::StyleSheetIdentifier const&);

template<>
ErrorOr<Web::CSS::StyleSheetIdentifier> decode(Decoder&);

}
