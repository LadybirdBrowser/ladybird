/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibIPC/Forward.h>

namespace HTTP::HSTS {

// https://www.rfc-editor.org/rfc/rfc6797#section-6.1
struct ParsedHSTSPolicy {
    // https://www.rfc-editor.org/rfc/rfc6797#section-6.1.1
    AK::Duration max_age { AK::Duration::zero() };

    // https://www.rfc-editor.org/rfc/rfc6797#section-6.1.2
    bool include_sub_domains { false };
};

Optional<ParsedHSTSPolicy> parse_header(StringView header_value);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, HTTP::HSTS::ParsedHSTSPolicy const&);

template<>
ErrorOr<HTTP::HSTS::ParsedHSTSPolicy> decode(Decoder&);

}
