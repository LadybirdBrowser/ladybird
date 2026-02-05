/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/RequestOrResponseBlocking.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

[[nodiscard]] WEB_API RequestOrResponseBlocking should_response_to_request_be_blocked_due_to_its_mime_type(Response const&, Request const&);

}
