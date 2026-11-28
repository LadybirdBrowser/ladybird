/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
[[nodiscard]] NonnullRefPtr<PolicyContainer> create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::ResponseView const> response, GC::Ptr<Environment> environment);

}
