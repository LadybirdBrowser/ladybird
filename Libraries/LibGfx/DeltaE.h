/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/CIELAB.h>

namespace Gfx {

// Returns a number between 0 and 100 that describes how far apart two colors are in human perception.
// A return value < 1 means that the two colors are not noticeably different.
// The larger the return value, the easier it is to tell the two colors apart.
// Works better for colors that are somewhat "close".
//
// You can use ICC::sRGB()->to_lab() to convert sRGB colors to CIELAB.
float DeltaE(CIELAB const&, CIELAB const&);

}
