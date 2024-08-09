/*
 * Copyright (c) 2024, Olekoop <mlglol360xd@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if defined(AK_OS_MACOS)
static_assert(false, "This file cannot be used for macOS");
#endif

#include <AK/Forward.h>
#include <AK/Function.h>
#include <EGL/egl.h>

namespace Core {

ErrorOr<void> create_egl_interface();

}
