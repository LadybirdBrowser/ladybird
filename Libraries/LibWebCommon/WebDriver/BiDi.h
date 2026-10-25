/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebDriver/Error.h>

namespace Web::WebDriver {

// js-uint = 0..9007199254740991
// https://w3c.github.io/webdriver-bidi/#protocol-definition
WEBCOMMON_API Optional<u64> as_js_uint(JsonValue const&);

// https://w3c.github.io/webdriver-bidi/#type-script-SerializationOptions
struct WEBCOMMON_API SerializationOptions {
    enum class IncludeShadowTree : u8 {
        None,
        Open,
        All,
    };

    // NB: An empty depth is null, which does not limit the depth.
    Optional<u64> max_dom_depth { 0 };
    Optional<u64> max_object_depth {};
    IncludeShadowTree include_shadow_tree { IncludeShadowTree::None };
};

WEBCOMMON_API ErrorOr<SerializationOptions, Error> parse_serialization_options(JsonValue const&);

// https://w3c.github.io/webdriver-bidi/#preload-script
struct WEBCOMMON_API BiDiPreloadScript {
    String id;
    String function_declaration;
    JsonValue arguments;
    Optional<Vector<String>> contexts;
    Optional<String> sandbox;
    Vector<String> user_contexts;
};

// The state of the BiDi session that processes hosting web content need: which events some subscription enables, and
// the preload scripts to run in new windows.
struct WEBCOMMON_API BiDiSessionState {
    AK_ALLOC_WITH_KMALLOC;

    bool is_active { false };
    Vector<String> enabled_events;
    Vector<BiDiPreloadScript> preload_scripts;
};

}

#include <LibWebCommon/WebDriver/BiDiCodecs.h>
