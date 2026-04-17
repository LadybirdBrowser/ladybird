/*
 * Copyright (c) 2026, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibURL/Forward.h>

namespace Web::Fetch::Infrastructure {

struct AuthenticationEntry {
    String username {};
    String password {};
    String realm {};
};

static HashMap<String, AuthenticationEntry> s_authentication_entries {};

void set_authentication_entry(URL::URL const&, AuthenticationEntry);
Optional<AuthenticationEntry> get_authentication_entry(URL::URL const&);

}
