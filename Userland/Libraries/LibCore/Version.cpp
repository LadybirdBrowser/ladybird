/*
 * Copyright (c) 2021, Mahmoud Mandour <ma.mandourr@gmail.com>
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/GitHash.h>
#include <LibCore/Version.h>

namespace Core::Version {

ErrorOr<String> read_long_version_string()
{
#if defined(GIT_HASH)
    return MUST(String::formatted("Version 1.0-{}", GIT_HASH));
#else
    return "Version 1.0"_string;
#endif
}

}
