/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatterninit
struct Init {
    Optional<String> protocol;
    Optional<String> username;
    Optional<String> password;
    Optional<String> hostname;
    Optional<String> port;
    Optional<String> pathname;
    Optional<String> search;
    Optional<String> hash;
    Optional<String> base_url;
};

}
