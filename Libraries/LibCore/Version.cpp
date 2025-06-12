/*
 * Copyright (c) 2021, Mahmoud Mandour <ma.mandourr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/Version.h>

namespace Core::Version {

String read_long_version_string()
{
    return "Version 1.0"_string;
}

}
