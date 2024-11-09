/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Fetch/Infrastructure/RequestOrResponseBlocking.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

[[nodiscard]] RequestOrResponseBlocking block_bad_port(Request const&);
[[nodiscard]] bool is_bad_port(u16);

}
