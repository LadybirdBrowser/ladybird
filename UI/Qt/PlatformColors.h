/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>

namespace Ladybird {

Gfx::Color macos_web_selection_color(Web::CSS::PreferredColorScheme);
Gfx::Color macos_web_inactive_selection_color();

}
