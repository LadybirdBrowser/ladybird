/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment
// The URLs and top-level origin of an environment, as plain data.
struct SerializedEnvironment {
    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-creation-url
    URL::URL creation_url;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-creation-url
    Optional<URL::URL> top_level_creation_url;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-origin
    Optional<URL::Origin> top_level_origin;
};

}
