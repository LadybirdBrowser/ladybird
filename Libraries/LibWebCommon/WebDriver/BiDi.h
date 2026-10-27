/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebDriver/Error.h>
#include <LibWebCommon/WebDriver/Network.h>

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
    // NB: A null scope enables the event globally; otherwise it names top-level traversables.
    HashMap<String, Optional<Vector<String>>> enabled_event_contexts;
    Vector<BiDiPreloadScript> preload_scripts;
    Vector<Network::Intercept> network_intercepts;
    Vector<Network::Header> network_default_headers;
    HashMap<String, Vector<Network::Header>> network_user_context_headers;
    HashMap<String, Vector<Network::Header>> network_navigable_headers;
    String network_default_cache_behavior { "default"_string };
    HashMap<String, String> network_navigable_cache_behavior;
};

}

#include <LibWebCommon/WebDriver/BiDiCodecs.h>
