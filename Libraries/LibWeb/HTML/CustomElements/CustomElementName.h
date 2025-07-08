/*
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::HTML {

bool is_valid_custom_element_name(String const& name);

inline bool is_valid_custom_element_name(FlyString const& name)
{
    return is_valid_custom_element_name(name.to_string());
}

}
