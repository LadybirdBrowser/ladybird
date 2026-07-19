/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

WEB_API void* clone_rust_style_group(size_t group_index, void const* payload);
WEB_API void free_rust_style_group(size_t group_index, void* payload);

}
