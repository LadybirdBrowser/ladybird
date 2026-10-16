/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibURL/Forward.h>
#include <LibWeb/Fetch/Infrastructure/RequestOrResponseBlocking.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>

namespace Web::Fetch::Fetching {

[[nodiscard]] bool cors_check(Infrastructure::Request const&, Infrastructure::Response const&);
[[nodiscard]] bool tao_check(Infrastructure::Request const&, Infrastructure::Response const&);
[[nodiscard]] bool navigation_tao_check(Infrastructure::Response const&, URL::Origin const& destination_origin);

enum class ForNavigation : u8 {
    No,
    Yes,
};

[[nodiscard]] Infrastructure::RequestOrResponseBlocking cross_origin_resource_policy_check(URL::Origin const&, HTML::EmbedderPolicy const&, Infrastructure::Response const&, ForNavigation = ForNavigation::No);

}
