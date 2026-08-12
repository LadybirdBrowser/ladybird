/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES3/gl3.h>

#include <AK/Checked.h>
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

static Optional<size_t> component_count_for_format(GLenum format)
{
    switch (format) {
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_DEPTH_COMPONENT:
        return 1;
    case GL_LUMINANCE_ALPHA:
    case GL_RG:
    case GL_RG_INTEGER:
        return 2;
    case GL_RGB:
    case GL_RGB_INTEGER:
        return 3;
    case GL_RGBA:
    case GL_RGBA_INTEGER:
        return 4;
    default:
        return {};
    }
}

static Optional<size_t> bytes_per_pixel(GLenum format, GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        break;
    }

    auto component_count = component_count_for_format(format);
    if (!component_count.has_value())
        return {};

    size_t component_size;
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        component_size = 1;
        break;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        component_size = 2;
        break;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
        component_size = 4;
        break;
    default:
        return {};
    }

    return *component_count * component_size;
}

bool is_valid_2d_pixel_unpack_state(GLsizei width, PixelUnpackState const& unpack_state)
{
    if (width < 0)
        return true;

    auto data_store_width = unpack_state.row_length == 0 ? static_cast<size_t>(width) : unpack_state.row_length;
    Checked<size_t> unpacked_row_end = unpack_state.skip_pixels;
    unpacked_row_end += static_cast<size_t>(width);
    return !unpacked_row_end.has_overflow() && unpacked_row_end.value() <= data_store_width;
}

Optional<size_t> required_2d_texture_data_size(GLsizei width, GLsizei height, GLenum format, GLenum type, PixelUnpackState const& unpack_state)
{
    if (width < 0 || height < 0)
        return {};
    if (!is_valid_2d_pixel_unpack_state(width, unpack_state))
        return {};
    if (width == 0 || height == 0)
        return 0;

    auto pixel_size = bytes_per_pixel(format, type);
    if (!pixel_size.has_value())
        return {};

    if (unpack_state.alignment != 1 && unpack_state.alignment != 2 && unpack_state.alignment != 4 && unpack_state.alignment != 8)
        return {};

    Checked<size_t> row_byte_length = unpack_state.row_length == 0 ? static_cast<size_t>(width) : unpack_state.row_length;
    row_byte_length *= *pixel_size;
    row_byte_length += unpack_state.alignment - 1;
    if (row_byte_length.has_overflow())
        return {};

    row_byte_length = row_byte_length.value() / unpack_state.alignment * unpack_state.alignment;

    Checked<size_t> skipped_rows_size = unpack_state.skip_rows;
    skipped_rows_size *= row_byte_length.value();

    Checked<size_t> skipped_pixels_size = unpack_state.skip_pixels;
    skipped_pixels_size *= *pixel_size;

    Checked<size_t> remaining_rows_size = static_cast<size_t>(height) - 1;
    remaining_rows_size *= row_byte_length.value();

    Checked<size_t> final_row_size = static_cast<size_t>(width);
    final_row_size *= *pixel_size;

    if (skipped_rows_size.has_overflow() || skipped_pixels_size.has_overflow() || remaining_rows_size.has_overflow() || final_row_size.has_overflow())
        return {};

    Checked<size_t> required_size = skipped_rows_size.value();
    required_size += skipped_pixels_size.value();
    required_size += remaining_rows_size.value();
    required_size += final_row_size.value();
    if (required_size.has_overflow())
        return {};

    return required_size.value();
}

}
