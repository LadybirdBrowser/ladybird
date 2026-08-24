/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

Vector<ComponentValue> unresolved_style_value_components(UnresolvedStyleValue const&);

}
