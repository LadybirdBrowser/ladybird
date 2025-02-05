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
#include <LibIPC/Forward.h>
#include <LibWeb/WebDriver/Response.h>

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
    Prompt,
    FallbackDefault,
};

// https://w3c.github.io/webdriver/#dfn-prompt-handler-configuration
struct PromptHandlerConfiguration {
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

UserPromptHandler const& user_prompt_handler();
void set_user_prompt_handler(UserPromptHandler);

Response deserialize_as_an_unhandled_prompt_behavior(JsonValue);
bool check_user_prompt_handler_matches(JsonObject const&);
void update_the_user_prompt_handler(JsonObject const&);
JsonValue serialize_the_user_prompt_handler();

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::WebDriver::PromptHandlerConfiguration const&);

template<>
ErrorOr<Web::WebDriver::PromptHandlerConfiguration> decode(Decoder&);

}
