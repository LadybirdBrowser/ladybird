/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace WebView {

// NB: A child ID names one helper process for as long as the browser runs. Unlike a PID, it is never reused for a
//     later process. Child ID zero denotes the browser itself.
struct ChildIdentity {
    u64 child_id { 0 };
    pid_t pid { -1 };
};

}
