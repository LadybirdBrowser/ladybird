/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

struct ErrorCodeData {
    ErrorCode error_code;
    unsigned http_status;
    String json_error_code;
};

// https://w3c.github.io/webdriver/#dfn-error-code
static Vector<ErrorCodeData> const s_error_code_data = {
    { ErrorCode::ElementClickIntercepted, 400, "element click intercepted"_string },
    { ErrorCode::ElementNotInteractable, 400, "element not interactable"_string },
    { ErrorCode::InsecureCertificate, 400, "insecure certificate"_string },
    { ErrorCode::InvalidArgument, 400, "invalid argument"_string },
    { ErrorCode::InvalidCookieDomain, 400, "invalid cookie domain"_string },
    { ErrorCode::InvalidElementState, 400, "invalid element state"_string },
    { ErrorCode::InvalidSelector, 400, "invalid selector"_string },
    { ErrorCode::InvalidSessionId, 404, "invalid session id"_string },
    { ErrorCode::JavascriptError, 500, "javascript error"_string },
    { ErrorCode::MoveTargetOutOfBounds, 500, "move target out of bounds"_string },
    { ErrorCode::NoSuchAlert, 404, "no such alert"_string },
    { ErrorCode::NoSuchCookie, 404, "no such cookie"_string },
    { ErrorCode::NoSuchElement, 404, "no such element"_string },
    { ErrorCode::NoSuchFrame, 404, "no such frame"_string },
    { ErrorCode::NoSuchWindow, 404, "no such window"_string },
    { ErrorCode::NoSuchShadowRoot, 404, "no such shadow root"_string },
    { ErrorCode::ScriptTimeoutError, 500, "script timeout"_string },
    { ErrorCode::SessionNotCreated, 500, "session not created"_string },
    { ErrorCode::StaleElementReference, 404, "stale element reference"_string },
    { ErrorCode::DetachedShadowRoot, 404, "detached shadow root"_string },
    { ErrorCode::Timeout, 500, "timeout"_string },
    { ErrorCode::UnableToSetCookie, 500, "unable to set cookie"_string },
    { ErrorCode::UnableToCaptureScreen, 500, "unable to capture screen"_string },
    { ErrorCode::UnexpectedAlertOpen, 500, "unexpected alert open"_string },
    { ErrorCode::UnknownCommand, 404, "unknown command"_string },
    { ErrorCode::UnknownError, 500, "unknown error"_string },
    { ErrorCode::UnknownMethod, 405, "unknown method"_string },
    { ErrorCode::UnsupportedOperation, 500, "unsupported operation"_string },
    { ErrorCode::OutOfMemory, 500, "out of memory"_string },
};

Error Error::from_code(ErrorCode code, String message, Optional<JsonValue> data)
{
    auto const& error_code_data = s_error_code_data[to_underlying(code)];

    return {
        error_code_data.http_status,
        error_code_data.json_error_code,
        move(message),
        move(data)
    };
}

Error Error::from_code(ErrorCode code, StringView message, Optional<JsonValue> data)
{
    return from_code(code, String::from_utf8_without_validation(message.bytes()), move(data));
}

Error::Error(AK::Error const& error)
{
    VERIFY(error.code() == ENOMEM);
    *this = from_code(ErrorCode::OutOfMemory, String {}, {});
}

Error::Error(unsigned http_status_, String error_, String message_, Optional<JsonValue> data_)
    : http_status(http_status_)
    , error(move(error_))
    , message(move(message_))
    , data(move(data_))
{
}

}
