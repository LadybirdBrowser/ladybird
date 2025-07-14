/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>

inline ErrorOr<String> temp_file_path(StringView temp_file)
{
#if !defined(AK_OS_WINDOWS)
    return String::formatted("/tmp/{}", temp_file);
#else
    return String::formatted("C:\\Windows\\Temp\\{}"sv, temp_file);
#endif
}
