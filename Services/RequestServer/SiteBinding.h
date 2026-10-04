/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace RequestServer {

// Whether a client may make requests with any network isolation key, or only with the keys of the sites that the UI
// process binds it to. Only the UI process's own clients are unrestricted.
enum class SiteBinding : u8 {
    Unrestricted,
    Bound,
};

}
