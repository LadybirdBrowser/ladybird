/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web::DOMURL {

// https://url.spec.whatwg.org/#concept-url-parser
WEB_API Optional<URL::URL> parse_from_byte_string(StringView input, Optional<URL::URL const&> base_url = {}, Optional<StringView> encoding = {});
WEB_API Optional<URL::URL> parse(Utf16View input, Optional<URL::URL const&> base_url = {}, Optional<Utf16View> encoding = {});

}
