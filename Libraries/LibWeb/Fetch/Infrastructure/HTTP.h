/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Fetch/Infrastructure/RedirectTaint.h>

namespace Web::Fetch::Infrastructure {

[[nodiscard]] ByteString default_user_agent_value(URL::URL const&);

}
