/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace HTTP::Cookie {

enum class IncludeCredentials : u8 {
    No,
    Yes,
};

}
