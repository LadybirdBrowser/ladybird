/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/JsonValue.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-error-code
enum class ErrorCode {
    ElementClickIntercepted,
    ElementNotInteractable,
    InsecureCertificate,
    InvalidArgument,
    InvalidCookieDomain,
    InvalidElementState,
    InvalidSelector,
    InvalidSessionId,
    JavascriptError,
    MoveTargetOutOfBounds,
    NoSuchAlert,
    NoSuchCookie,
    NoSuchElement,
    NoSuchFrame,
    NoSuchWindow,
    NoSuchShadowRoot,
    ScriptTimeoutError,
    SessionNotCreated,
    StaleElementReference,
    DetachedShadowRoot,
    Timeout,
    UnableToSetCookie,
    UnableToCaptureScreen,
    UnexpectedAlertOpen,
    UnknownCommand,
    UnknownError,
    UnknownMethod,
    UnsupportedOperation,

    // Non-standard error codes:
    OutOfMemory,
};

// https://w3c.github.io/webdriver/#errors
struct Error {
    unsigned http_status;
    ByteString error;
    ByteString message;
    Optional<JsonValue> data;

    static Error from_code(ErrorCode, ByteString message, Optional<JsonValue> data = {});

    Error(unsigned http_status, ByteString error, ByteString message, Optional<JsonValue> data);
    Error(AK::Error const&);
};

}

template<>
struct AK::Formatter<Web::WebDriver::Error> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::WebDriver::Error const& error)
    {
        return Formatter<StringView>::format(builder, ByteString::formatted("Error {}, {}: {}", error.http_status, error.error, error.message));
    }
};
