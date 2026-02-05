/*
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Web::CSS {

enum class PreferredColorScheme {
    Auto,
    Dark,
    Light,
};

PreferredColorScheme preferred_color_scheme_from_string(StringView);
StringView preferred_color_scheme_to_string(PreferredColorScheme);

}
