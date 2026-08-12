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
WEB_API bool is_valid_2d_pixel_unpack_state(GLsizei width, PixelUnpackState const&);
WEB_API Optional<size_t> required_2d_texture_data_size(GLsizei width, GLsizei height, GLenum format, GLenum type, PixelUnpackState const&);

}
