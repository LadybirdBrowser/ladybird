/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>

#include <AK/Debug.h>
#include <LibWeb/WebGL/TextureUpload.h>

namespace Web::WebGL {

Optional<Gfx::ExportFormat> texture_export_format(GLenum format, GLenum type)
{
    switch (format) {
    case GL_RGB:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::RGB888;
        case GL_UNSIGNED_SHORT_5_6_5:
            return Gfx::ExportFormat::RGB565;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::RGBA8888;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            // FIXME: This is not exactly the same as RGBA.
            return Gfx::ExportFormat::RGBA4444;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return Gfx::ExportFormat::RGBA5551;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::Alpha8;
        default:
            break;
        }
        break;
    case GL_LUMINANCE:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::Gray8;
        default:
            break;
        }
        break;
    default:
        break;
    }

    dbgln("WebGL: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
    return {};
}

}
