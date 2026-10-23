/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/URLSearchParams.h>
#include <LibWeb/Export.h>

namespace Web::DOMURL {

struct QueryParam {
    Utf16String name;
    Utf16String value;
};
String url_encode(Vector<QueryParam> const&, StringView encoding = "UTF-8"sv);
Vector<QueryParam> url_decode(StringView);

class WEB_API URLSearchParams final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(URLSearchParams, url_search_params)

public:
    String serialize_to_byte_string() const;
};

}
