/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy {

[[nodiscard]] Directives::Directive::Result should_request_be_blocked_by_content_security_policy(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request>);

}
