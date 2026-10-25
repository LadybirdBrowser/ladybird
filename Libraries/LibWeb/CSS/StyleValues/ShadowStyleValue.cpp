/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>

namespace Web::CSS {

// The type and placement discriminants cross the style value FFI as raw codes; the Rust
// serializer's tables depend on them.
static_assert(to_underlying(ShadowStyleValue::ShadowType::Normal) == 0);
static_assert(to_underlying(ShadowStyleValue::ShadowType::Text) == 1);
static_assert(to_underlying(ShadowPlacement::Outer) == 0);
static_assert(to_underlying(ShadowPlacement::Inner) == 1);

}
