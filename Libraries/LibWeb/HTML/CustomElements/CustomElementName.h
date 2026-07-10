/*
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>

namespace Web::HTML {

bool is_valid_custom_element_name(Utf16View const& name);

inline bool is_valid_custom_element_name(Utf16FlyString const& name)
{
    return is_valid_custom_element_name(name.view());
}

}
