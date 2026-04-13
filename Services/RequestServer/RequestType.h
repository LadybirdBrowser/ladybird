/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace RequestServer {

enum class RequestType : u8 {
    Fetch,
    Connect,
    BackgroundRevalidation,
};

}
