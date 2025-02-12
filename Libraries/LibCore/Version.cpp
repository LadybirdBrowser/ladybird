/*
 * Copyright (c) 2021, Mahmoud Mandour <ma.mandourr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/Environment.h>
#include <LibCore/Version.h>

namespace Core::Version {

String read_long_version_string()
{
    auto validate_git_hash = [](auto hash) {
        if (hash.length() < 4 || hash.length() > 40)
            return false;
        for (auto ch : hash) {
            if (!is_ascii_hex_digit(ch))
                return false;
        }
        return true;
    };

    auto maybe_git_hash = Core::Environment::get("LADYBIRD_GIT_VERSION"sv);

    if (maybe_git_hash.has_value() && validate_git_hash(maybe_git_hash.value()))
        return MUST(String::formatted("Version 1.0-{}", maybe_git_hash.value()));
    return "Version 1.0"_string;
}

}
