/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/JsonObject.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/UserPrompt.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-user-prompt-handler
static UserPromptHandler s_user_prompt_handler;

// https://w3c.github.io/webdriver/#dfn-known-prompt-handlers
static constexpr Array known_prompt_handlers { "dismiss"sv, "accept"sv, "dismiss and notify"sv, "accept and notify"sv, "ignore"sv };

// https://w3c.github.io/webdriver/#dfn-valid-prompt-types
static constexpr Array valid_prompt_types { "alert"sv, "beforeUnload"sv, "confirm"sv, "default"sv, "prompt"sv };

static constexpr PromptHandler prompt_handler_from_string(StringView prompt_handler)
{
    if (prompt_handler == "dismiss"sv)
        return PromptHandler::Dismiss;
    if (prompt_handler == "accept"sv)
        return PromptHandler::Accept;
    if (prompt_handler == "ignore"sv)
        return PromptHandler::Ignore;
    VERIFY_NOT_REACHED();
}

static constexpr PromptType prompt_type_from_string(StringView prompt_type)
{
    if (prompt_type == "alert"sv)
        return PromptType::Alert;
    if (prompt_type == "beforeUnload"sv)
        return PromptType::BeforeUnload;
    if (prompt_type == "confirm"sv)
        return PromptType::Confirm;
    if (prompt_type == "default"sv)
        return PromptType::Default;
    if (prompt_type == "prompt"sv)
        return PromptType::Prompt;
    if (prompt_type == "fallbackDefault"sv)
        return PromptType::FallbackDefault;
    VERIFY_NOT_REACHED();
}

PromptHandlerConfiguration PromptHandlerConfiguration::deserialize(JsonValue const& configuration)
{
    auto handler = prompt_handler_from_string(*configuration.as_object().get_byte_string("handler"sv));

    auto notify = *configuration.as_object().get_bool("notify"sv)
        ? PromptHandlerConfiguration::Notify::Yes
        : PromptHandlerConfiguration::Notify::No;

    return { .handler = handler, .notify = notify };
}

UserPromptHandler const& user_prompt_handler()
{
    return s_user_prompt_handler;
}

void set_user_prompt_handler(UserPromptHandler user_prompt_handler)
{
    s_user_prompt_handler = move(user_prompt_handler);
}

// https://w3c.github.io/webdriver/#dfn-deserialize-as-an-unhandled-prompt-behavior
Response deserialize_as_an_unhandled_prompt_behavior(JsonValue value)
{
    // 1. Set value to the result of converting a JSON-derived JavaScript value to an Infra value with value.
    // 2. If value is not a string, an implementation that does not also support [WebDriver-BiDi] may return error with
    //    error code invalid argument.

    // 3. Let is string value be false.
    bool is_string_value = false;

    // 3. If value is a string set value to the map «["fallbackDefault" → value]» and set is string value to true.
    if (value.is_string()) {
        JsonObject map;
        map.set("fallbackDefault"sv, move(value));

        value = move(map);
        is_string_value = true;
    }

    // 4. If value is not a map return error with error code invalid argument.
    if (!value.is_object())
        return WebDriver::Error::from_code(ErrorCode::InvalidArgument, "Capability unhandledPromptBehavior must be a string or object"sv);

    // 5. Let user prompt handler be an empty map.
    JsonObject user_prompt_handler;

    // 6. For each prompt type → handler in value:
    TRY(value.as_object().try_for_each_member([&](ByteString const& prompt_type, JsonValue const& handler_value) -> ErrorOr<void, WebDriver::Error> {
        // 1. If is string value is false and valid prompt types does not contain prompt type return error with error code invalid argument.
        if (!is_string_value && !valid_prompt_types.contains_slow(prompt_type))
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("'{}' is not a valid prompt type", prompt_type));

        // 2. If known prompt handlers does not contain an entry with handler key handler return error with error code invalid argument.
        if (!handler_value.is_string())
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, "Prompt handler must be a string"sv);

        StringView handler = handler_value.as_string();

        if (!known_prompt_handlers.contains_slow(handler))
            return WebDriver::Error::from_code(ErrorCode::InvalidArgument, ByteString::formatted("'{}' is not a known prompt handler", handler));

        // 3. Let notify be false.
        bool notify = false;

        // 4. If handler is "accept and notify", set handler to "accept" and notify to true.
        if (handler == "accept and notify"sv) {
            handler = "accept"sv;
            notify = true;
        }

        // 5. If handler is "dismiss and notify", set handler to "dismiss" and notify to true.
        else if (handler == "dismiss and notify"sv) {
            handler = "dismiss"sv;
            notify = true;
        }

        // 6. If handler is "ignore", set notify to true.
        else if (handler == "ignore"sv) {
            notify = true;
        }

        // 7. Let configuration be a prompt handler configuration with handler handler and notify notify.
        JsonObject configuration;
        configuration.set("handler"sv, handler);
        configuration.set("notify"sv, notify);

        // 8. Set user prompt handler[prompt type] to configuration.
        user_prompt_handler.set(prompt_type, move(configuration));

        return {};
    }));

    // Return success with data user prompt handler.
    return JsonValue { move(user_prompt_handler) };
}

// https://w3c.github.io/webdriver/#dfn-update-the-user-prompt-handler
void update_the_user_prompt_handler(JsonObject const& requested_prompt_handler)
{
    // 1. If the user prompt handler is null, set the user prompt handler to an empty map.
    if (!s_user_prompt_handler.has_value())
        s_user_prompt_handler = UserPromptHandler::ValueType {};

    // 2. For each request prompt type → request handler in requested prompt handler:
    requested_prompt_handler.for_each_member([&](ByteString const& request_prompt_type, JsonValue const& request_handler) {
        // 1. Set user prompt handler[request prompt type] to request handler.
        s_user_prompt_handler->set(
            prompt_type_from_string(request_prompt_type),
            PromptHandlerConfiguration::deserialize(request_handler));
    });
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebDriver::PromptHandlerConfiguration const& configuration)
{
    TRY(encoder.encode(configuration.handler));
    TRY(encoder.encode(configuration.notify));

    return {};
}

template<>
ErrorOr<Web::WebDriver::PromptHandlerConfiguration> IPC::decode(Decoder& decoder)
{
    auto handler = TRY(decoder.decode<Web::WebDriver::PromptHandler>());
    auto notify = TRY(decoder.decode<Web::WebDriver::PromptHandlerConfiguration::Notify>());

    return Web::WebDriver::PromptHandlerConfiguration { .handler = handler, .notify = notify };
}
