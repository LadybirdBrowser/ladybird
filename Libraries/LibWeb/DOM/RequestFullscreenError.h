/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Types.h>

namespace Web::DOM {

enum class RequestFullscreenError : u8 {
    False,
    ElementReadyCheckFailed,
    UnsupportedElement,
    NoTransientUserActivation,
    ElementNodeDocIsNotPendingDoc,
};

inline StringView request_fullscreen_error_to_string(RequestFullscreenError error)
{
    switch (error) {
    case RequestFullscreenError::False:
        break;
    case RequestFullscreenError::ElementReadyCheckFailed:
        return "Element ready check failed"sv;
    case RequestFullscreenError::UnsupportedElement:
        return "Not supported element"sv;
    case RequestFullscreenError::NoTransientUserActivation:
        return "No transient user activation available to consume"sv;
    case RequestFullscreenError::ElementNodeDocIsNotPendingDoc:
        return "Element's node document is not pending doc"sv;
    }
    VERIFY_NOT_REACHED();
}

}
