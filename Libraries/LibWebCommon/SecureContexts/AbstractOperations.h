/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::SecureContexts {

enum class Trustworthiness {
    PotentiallyTrustworthy,
    NotTrustworthy,
};

[[nodiscard]] WEBCOMMON_API Trustworthiness is_origin_potentially_trustworthy(URL::Origin const&);
[[nodiscard]] WEBCOMMON_API Trustworthiness is_url_potentially_trustworthy(URL::URL const&);

}
