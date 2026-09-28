/*
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibWebCommon/Export.h>

namespace Web::CSS {

enum class PreferredColorScheme {
    Auto,
    Dark,
    Light,
};

WEBCOMMON_API PreferredColorScheme preferred_color_scheme_from_string(Utf16View);
WEBCOMMON_API Utf16FlyString preferred_color_scheme_to_utf16_fly_string(PreferredColorScheme);

}
