/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/AnonymousBuffer.h>

namespace Ladybird {

bool is_using_dark_system_theme();
Core::AnonymousBuffer create_system_palette();

}
