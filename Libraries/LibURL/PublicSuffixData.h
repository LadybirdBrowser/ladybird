/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <LibURL/Forward.h>

namespace URL {

class PublicSuffixData {
public:
    static bool is_matching_public_suffix(StringView host);
    static bool is_matching_public_suffix(Host const& host);
    static Optional<String> find_matching_public_suffix(StringView string);
    static Optional<String> find_matching_public_suffix(Host const& host);
    static Optional<String> find_matching_registrable_domain(StringView string);
    static Optional<String> find_matching_registrable_domain(Host const& host);
};

}
