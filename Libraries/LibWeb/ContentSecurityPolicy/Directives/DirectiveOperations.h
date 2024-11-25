/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

enum class ShouldExecute {
    No,
    Yes,
};

[[nodiscard]] Optional<FlyString> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request);
[[nodiscard]] Vector<StringView> get_fetch_directive_fallback_list(Optional<FlyString> directive_name);
[[nodiscard]] ShouldExecute should_fetch_directive_execute(Optional<FlyString> effective_directive_name, FlyString const& directive_name, GC::Ref<Policy const> policy);

}
