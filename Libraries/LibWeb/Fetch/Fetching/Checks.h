/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibURL/Forward.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

[[nodiscard]] bool cors_check(Infrastructure::Request const&, Infrastructure::Response const&);
[[nodiscard]] bool tao_check(Infrastructure::Request const&, Infrastructure::Response const&);
[[nodiscard]] bool navigation_tao_check(Infrastructure::Response const&, URL::Origin const& destination_origin);

enum class ForNavigation {
    No,
    Yes,
};
[[nodiscard]] bool cross_origin_resource_policy_check(URL::Origin const&, GC::Ptr<HTML::EnvironmentSettingsObject const> environment, Optional<Infrastructure::Request::Destination>, Infrastructure::Response const&, ForNavigation = ForNavigation::No);

}
