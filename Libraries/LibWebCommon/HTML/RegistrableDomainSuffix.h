/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibURL/Host.h>
#include <LibWebCommon/Export.h>

namespace Web::DOM {

// https://html.spec.whatwg.org/multipage/browsers.html#is-a-registrable-domain-suffix-of-or-is-equal-to
WEBCOMMON_API bool is_a_registrable_domain_suffix_of_or_is_equal_to(Utf16View host_suffix_string, URL::Host const& original_host);
WEBCOMMON_API bool is_a_registrable_domain_suffix_of_or_is_equal_to(URL::Host const& host_suffix, URL::Host const& original_host);

}
