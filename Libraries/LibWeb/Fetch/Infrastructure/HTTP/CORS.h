/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibHTTP/Forward.h>

namespace Web::Fetch::Infrastructure {

[[nodiscard]] bool is_cors_safelisted_request_header(HTTP::Header const&);
[[nodiscard]] bool is_cors_unsafe_request_header_byte(u8);
[[nodiscard]] Vector<ByteString> get_cors_unsafe_header_names(HTTP::HeaderList const&);
[[nodiscard]] bool is_cors_non_wildcard_request_header_name(StringView);
[[nodiscard]] bool is_privileged_no_cors_request_header_name(StringView);
[[nodiscard]] bool is_cors_safelisted_response_header_name(StringView, ReadonlySpan<StringView>);
[[nodiscard]] bool is_no_cors_safelisted_request_header_name(StringView);
[[nodiscard]] bool is_no_cors_safelisted_request_header(HTTP::Header const&);

}
