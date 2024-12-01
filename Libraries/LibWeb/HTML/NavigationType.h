/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

// FIXME: Generate this from the IDL file that just has an enum in it
namespace Web::Bindings {
enum class NavigationType {
    Push,
    Replace,
    Reload,
    Traverse,
};

inline String idl_enum_to_string(NavigationType value)
{
    switch (value) {
    case NavigationType::Push:
        return "push"_string;
    case NavigationType::Replace:
        return "replace"_string;
    case NavigationType::Reload:
        return "reload"_string;
    case NavigationType::Traverse:
        return "traverse"_string;
    default:
        return "<unknown>"_string;
    }
}

}
