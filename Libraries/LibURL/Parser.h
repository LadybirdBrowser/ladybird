/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibTextCodec/Encoder.h>
#include <LibURL/URL.h>

namespace URL {

#define ENUMERATE_STATES                 \
    STATE(SchemeStart)                   \
    STATE(Scheme)                        \
    STATE(NoScheme)                      \
    STATE(SpecialRelativeOrAuthority)    \
    STATE(PathOrAuthority)               \
    STATE(Relative)                      \
    STATE(RelativeSlash)                 \
    STATE(SpecialAuthoritySlashes)       \
    STATE(SpecialAuthorityIgnoreSlashes) \
    STATE(Authority)                     \
    STATE(Host)                          \
    STATE(Hostname)                      \
    STATE(Port)                          \
    STATE(File)                          \
    STATE(FileSlash)                     \
    STATE(FileHost)                      \
    STATE(PathStart)                     \
    STATE(Path)                          \
    STATE(CannotBeABaseUrlPath)          \
    STATE(Query)                         \
    STATE(Fragment)

class Parser {
public:
    enum class State {
#define STATE(state) state,
        ENUMERATE_STATES
#undef STATE
    };

    static char const* state_name(State const& state)
    {
        switch (state) {
#define STATE(state)   \
    case State::state: \
        return #state;
            ENUMERATE_STATES
#undef STATE
        }
        VERIFY_NOT_REACHED();
    }

    // https://url.spec.whatwg.org/#concept-basic-url-parser
    static URL basic_parse(StringView input, Optional<URL> const& base_url = {}, URL* url = nullptr, Optional<State> state_override = {}, Optional<StringView> encoding = {});

    // https://url.spec.whatwg.org/#string-percent-encode-after-encoding
    static String percent_encode_after_encoding(TextCodec::Encoder&, StringView input, PercentEncodeSet percent_encode_set, bool space_as_plus = false);

    // https://url.spec.whatwg.org/#concept-host-serializer
    static ErrorOr<String> serialize_host(Host const&);

    // https://url.spec.whatwg.org/#shorten-a-urls-path
    static void shorten_urls_path(URL&);
};

#undef ENUMERATE_STATES

}
