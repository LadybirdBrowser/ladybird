/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibTextCodec/Forward.h>
#include <LibURL/URL.h>

namespace URL {

class Parser {
public:
    enum class State {
        SchemeStart,
        Scheme,
        NoScheme,
        SpecialRelativeOrAuthority,
        PathOrAuthority,
        Relative,
        RelativeSlash,
        SpecialAuthoritySlashes,
        SpecialAuthorityIgnoreSlashes,
        Authority,
        Host,
        Hostname,
        Port,
        File,
        FileSlash,
        FileHost,
        PathStart,
        Path,
        OpaquePath,
        Query,
        Fragment,
    };

    // https://url.spec.whatwg.org/#concept-basic-url-parser
    static Optional<URL> basic_parse(StringView input, Optional<URL const&> base_url = {}, URL* url = nullptr, Optional<State> state_override = {}, Optional<StringView> encoding = {});

    // https://url.spec.whatwg.org/#string-percent-encode-after-encoding
    static String percent_encode_after_encoding(TextCodec::Encoder&, StringView input, PercentEncodeSet percent_encode_set, bool space_as_plus = false);

    static Optional<Host> parse_host(StringView input, bool is_opaque = false);
};

}
