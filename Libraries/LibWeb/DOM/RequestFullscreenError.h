/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16String.h>

namespace Web::DOM {

enum class RequestFullscreenError : u8 {
    False,
    ElementReadyCheckFailed,
    UnsupportedElement,
    NoTransientUserActivation,
    ElementNodeDocIsNotPendingDoc,
};

inline Utf16String request_fullscreen_error_to_string(RequestFullscreenError error)
{
    switch (error) {
    case RequestFullscreenError::False:
        break;
    case RequestFullscreenError::ElementReadyCheckFailed:
        return "Element ready check failed"_utf16;
    case RequestFullscreenError::UnsupportedElement:
        return "Not supported element"_utf16;
    case RequestFullscreenError::NoTransientUserActivation:
        return "No transient user activation available to consume"_utf16;
    case RequestFullscreenError::ElementNodeDocIsNotPendingDoc:
        return "Element's node document is not pending doc"_utf16;
    }
    VERIFY_NOT_REACHED();
}

}
