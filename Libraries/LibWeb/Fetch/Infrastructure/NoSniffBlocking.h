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

[[nodiscard]] bool determine_nosniff(HeaderList const&);
[[nodiscard]] RequestOrResponseBlocking should_response_to_request_be_blocked_due_to_nosniff(Response const&, Request const&);

}
