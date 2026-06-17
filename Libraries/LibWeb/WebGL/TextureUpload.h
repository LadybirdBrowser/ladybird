/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/BitmapExport.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

WEB_API Optional<Gfx::ExportFormat> texture_export_format(GLenum format, GLenum type);

}
