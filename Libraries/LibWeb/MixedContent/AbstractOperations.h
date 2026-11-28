/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/RequestOrResponseBlocking.h>

namespace Web::Fetch::Infrastructure {

class Request;
class Response;

}

namespace Web::MixedContent {

WEB_API void upgrade_a_mixed_content_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request&);

WEB_API Fetch::Infrastructure::RequestOrResponseBlocking should_fetching_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request&);

WEB_API Fetch::Infrastructure::RequestOrResponseBlocking should_response_to_request_be_blocked_as_mixed_content(Fetch::Infrastructure::Request&, Fetch::Infrastructure::Response const&);

}
