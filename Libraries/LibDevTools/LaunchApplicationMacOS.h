/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

#include <sys/types.h>

namespace DevTools {

// Brings up an application bundle through LaunchServices and hands back the process it came up as.
ErrorOr<pid_t> launch_macos_application(StringView bundle_path, Vector<ByteString> const& arguments);

}
