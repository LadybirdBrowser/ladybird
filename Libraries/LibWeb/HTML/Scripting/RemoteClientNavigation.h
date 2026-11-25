/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

namespace Web::HTML {

// A navigation whose request has a client another process hosts, by which the UI process finds that client again.
struct RemoteClientNavigation {
    CrossProcessId navigable_id;
    Utf16String navigation_id;
};

}
