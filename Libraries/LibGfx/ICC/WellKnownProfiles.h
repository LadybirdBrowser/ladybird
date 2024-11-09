/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>

namespace Gfx::ICC {

class Profile;
class TagData;

ErrorOr<NonnullRefPtr<Profile>> sRGB();

ErrorOr<NonnullRefPtr<TagData>> sRGB_curve();

}
