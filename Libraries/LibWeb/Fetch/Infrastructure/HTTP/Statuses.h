/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-status
// A status is an integer in the range 0 to 999, inclusive.
using Status = u16;

[[nodiscard]] bool is_null_body_status(Status);
[[nodiscard]] bool is_ok_status(Status);
[[nodiscard]] bool is_redirect_status(Status);

}
