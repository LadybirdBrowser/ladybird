/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/RustQueryHandle.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-conditional-3/#at-supports
WEB_API bool supports_condition_matches(RustQueryHandle const&);

}
