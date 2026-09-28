/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::SRI {

// https://w3c.github.io/webappsec-subresource-integrity/#integrity-policy
struct IntegrityPolicy {
    Vector<Utf16String> sources;
    Vector<Fetch::Infrastructure::Request::Destination> blocked_destinations;
    Vector<Utf16String> endpoints;

    bool is_empty() const { return sources.is_empty() && blocked_destinations.is_empty() && endpoints.is_empty(); }
};

// https://w3c.github.io/webappsec-subresource-integrity/#should-request-be-blocked-by-integrity-policy
ContentSecurityPolicy::Directives::Directive::Result should_request_be_blocked_by_integrity_policy(GC::Ref<Fetch::Infrastructure::Request>);

}
