/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#script-fetch-options
struct ScriptFetchOptions {
    AK_ALLOC_WITH_KMALLOC;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-nonce
    Utf16String cryptographic_nonce {};

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-integrity
    Utf16String integrity_metadata {};

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-parser
    Fetch::Infrastructure::Request::ParserMetadata parser_metadata { Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-credentials
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode { Fetch::Infrastructure::Request::CredentialsMode::SameOrigin };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-referrer-policy
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-render-blocking
    bool render_blocking { false };

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-fetch-options-fetch-priority
    Fetch::Infrastructure::Request::Priority fetch_priority {};
};

// https://html.spec.whatwg.org/multipage/webappapis.html#default-script-fetch-options
ScriptFetchOptions default_script_fetch_options();

}
