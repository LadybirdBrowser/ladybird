/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::ReferrerPolicy {

enum class OriginOnly {
    Yes,
    No,
};

ReferrerPolicy parse_a_referrer_policy_from_a_referrer_policy_header(Fetch::Infrastructure::Response const&);
void set_request_referrer_policy_on_redirect(Fetch::Infrastructure::Request&, Fetch::Infrastructure::Response const&);
Optional<URL::URL> determine_requests_referrer(Fetch::Infrastructure::Request const&);
Optional<URL::URL> strip_url_for_use_as_referrer(Optional<URL::URL>, OriginOnly origin_only = OriginOnly::No);

}
