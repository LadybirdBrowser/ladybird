/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace Web::Fetch::Infrastructure {

enum class RedirectTaint {
    SameOrigin,
    SameSite,
    CrossSite,
};

[[nodiscard]] ByteString const& default_user_agent_value();

}
