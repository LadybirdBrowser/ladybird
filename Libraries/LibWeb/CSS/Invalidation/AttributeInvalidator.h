/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>

namespace Web::DOM {

class Element;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_attribute_change(
    DOM::Element&,
    FlyString const& attribute_name,
    Optional<Utf16String> const& old_value,
    Optional<Utf16String> const& new_value);

}
