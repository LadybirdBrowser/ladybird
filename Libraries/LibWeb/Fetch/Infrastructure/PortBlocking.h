/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/RequestOrResponseBlocking.h>

namespace Web::Fetch::Infrastructure {

class Request;

[[nodiscard]] WEB_API RequestOrResponseBlocking block_bad_port(Request const&);

}
