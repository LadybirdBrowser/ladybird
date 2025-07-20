/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>

namespace Web::Painting {

WEB_API void paint_table_borders(DisplayListRecordingContext&, PaintableBox const& table_paintable);

}
