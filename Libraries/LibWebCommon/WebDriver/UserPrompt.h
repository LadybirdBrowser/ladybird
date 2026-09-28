/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebDriver/Response.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-known-prompt-handlers
enum class PromptHandler {
    Accept,
    Dismiss,
    Ignore,
};

// https://w3c.github.io/webdriver/#dfn-valid-prompt-types
enum class PromptType {
    Alert,
    BeforeUnload,
    Confirm,
    Default,
    File,
    Prompt,
    FallbackDefault,
};

// https://w3c.github.io/webdriver/#dfn-prompt-handler-configuration
struct WEBCOMMON_API PromptHandlerConfiguration {
    enum class Notify {
        No,
        Yes,
    };

    static PromptHandlerConfiguration deserialize(JsonValue const&);
    StringView serialize() const;

    bool operator==(PromptHandlerConfiguration const&) const = default;

    PromptHandler handler { PromptHandler::Dismiss };
    Notify notify { Notify::Yes };
};

// https://w3c.github.io/webdriver/#dfn-user-prompt-handler
using UserPromptHandler = Optional<HashMap<PromptType, PromptHandlerConfiguration>>;

WEBCOMMON_API UserPromptHandler const& user_prompt_handler();
WEBCOMMON_API void set_user_prompt_handler(UserPromptHandler);

WEBCOMMON_API PromptHandlerConfiguration get_the_prompt_handler(PromptType);
WEBCOMMON_API Response deserialize_as_an_unhandled_prompt_behavior(JsonValue);
WEBCOMMON_API bool check_user_prompt_handler_matches(JsonObject const&);
WEBCOMMON_API void update_the_user_prompt_handler(JsonObject const&);
WEBCOMMON_API JsonValue serialize_the_user_prompt_handler();

WEBCOMMON_API Error create_annotated_unexpected_alert_open_error(Optional<Utf16String> const& text);

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::WebDriver::PromptHandlerConfiguration const&);

template<>
WEBCOMMON_API ErrorOr<Web::WebDriver::PromptHandlerConfiguration> decode(Decoder&);

}
