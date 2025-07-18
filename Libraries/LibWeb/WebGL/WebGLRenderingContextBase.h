/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/BitmapExportResult.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Types.h>

#define SET_ERROR_VALUE_IF_ERROR(expression, error_value) \
    ({                                                    \
        auto maybe_error = expression;                    \
        if (maybe_error.is_error()) [[unlikely]] {        \
            set_error(error_value);                       \
            return;                                       \
        }                                                 \
        maybe_error.release_value();                      \
    })

namespace Web::WebGL {

static constexpr int COMPRESSED_TEXTURE_FORMATS = 0x86A3;
static constexpr int UNPACK_FLIP_Y_WEBGL = 0x9240;
static constexpr int UNPACK_PREMULTIPLY_ALPHA_WEBGL = 0x9241;
static constexpr int UNPACK_COLORSPACE_CONVERSION_WEBGL = 0x9243;
static constexpr int BROWSER_DEFAULT_WEBGL = 0x9244;
static constexpr int MAX_CLIENT_WAIT_TIMEOUT_WEBGL = 0x9247;

// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using TexImageSource = Variant<GC::Root<HTML::ImageBitmap>, GC::Root<HTML::ImageData>, GC::Root<HTML::HTMLImageElement>, GC::Root<HTML::HTMLCanvasElement>, GC::Root<HTML::OffscreenCanvas>, GC::Root<HTML::HTMLVideoElement>>;

class WebGLRenderingContextBase : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGLRenderingContextBase, Bindings::PlatformObject);

public:
    using Float32List = Variant<GC::Root<JS::Float32Array>, Vector<float>>;
    using Int32List = Variant<GC::Root<JS::Int32Array>, Vector<WebIDL::Long>>;
    using Uint32List = Variant<GC::Root<JS::Uint32Array>, Vector<WebIDL::UnsignedLong>>;

    virtual OpenGLContext& context() = 0;

protected:
    WebGLRenderingContextBase(JS::Realm&);

    virtual bool ext_texture_filter_anisotropic_extension_enabled() const = 0;
    virtual bool angle_instanced_arrays_extension_enabled() const = 0;
    virtual bool oes_standard_derivatives_extension_enabled() const = 0;
    virtual bool webgl_draw_buffers_extension_enabled() const = 0;
    virtual ReadonlySpan<WebIDL::UnsignedLong> enabled_compressed_texture_formats() const = 0;

    template<typename T>
    static ErrorOr<Span<T>> get_offset_span(Span<T> src_span, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        Checked<WebIDL::UnsignedLongLong> length = src_offset;
        length += src_length_override;
        if (length.has_overflow() || length.value_unchecked() > src_span.size()) [[unlikely]]
            return Error::from_errno(EINVAL);

        if (src_length_override == 0)
            return src_span.slice(src_offset, src_span.size() - src_offset);

        return src_span.slice(src_offset, src_length_override);
    }

    template<typename T>
    static ErrorOr<Span<T>> get_offset_span(GC::Ref<WebIDL::BufferableObjectBase> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        auto buffer_size = src_data->byte_length();
        if (buffer_size % sizeof(T) != 0) [[unlikely]]
            return Error::from_errno(EINVAL);

        auto raw_object = src_data->raw_object();

        if (auto* array_buffer = as_if<JS::ArrayBuffer>(*raw_object)) {
            return TRY(get_offset_span(array_buffer->buffer().span(), src_offset, src_length_override)).reinterpret<T>();
        }

        if (auto* data_view = as_if<JS::DataView>(*raw_object)) {
            return TRY(get_offset_span(data_view->viewed_array_buffer()->buffer().span(), src_offset, src_length_override)).reinterpret<T>();
        }

        // NOTE: This has to be done because src_offset is the number of elements to offset by, not the number of bytes.
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type)                         \
    if (auto* typed_array = as_if<JS::ClassName>(*raw_object)) {                                            \
        return TRY(get_offset_span(typed_array->data(), src_offset, src_length_override)).reinterpret<T>(); \
    }
        JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

        VERIFY_NOT_REACHED();
    }

    static ErrorOr<Span<float>> span_from_float32_list(Float32List& float32_list, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        if (float32_list.has<Vector<float>>()) {
            auto& vector = float32_list.get<Vector<float>>();
            return get_offset_span(vector.span(), src_offset, src_length_override);
        }
        auto& buffer = float32_list.get<GC::Root<JS::Float32Array>>();
        return get_offset_span(buffer->data(), src_offset, src_length_override);
    }

    static ErrorOr<Span<int>> span_from_int32_list(Int32List& int32_list, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        if (int32_list.has<Vector<int>>()) {
            auto& vector = int32_list.get<Vector<int>>();
            return get_offset_span(vector.span(), src_offset, src_length_override);
        }
        auto& buffer = int32_list.get<GC::Root<JS::Int32Array>>();
        return get_offset_span(buffer->data(), src_offset, src_length_override);
    }

    static ErrorOr<Span<u32>> span_from_uint32_list(Uint32List& uint32_list, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        if (uint32_list.has<Vector<u32>>()) {
            auto& vector = uint32_list.get<Vector<u32>>();
            return get_offset_span(vector.span(), src_offset, src_length_override);
        }
        auto& buffer = uint32_list.get<GC::Root<JS::Uint32Array>>();
        return get_offset_span(buffer->data(), src_offset, src_length_override);
    }

    Optional<Gfx::BitmapExportResult> read_and_pixel_convert_texture_image_source(TexImageSource const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width = OptionalNone {}, Optional<int> destination_height = OptionalNone {});

    static Vector<GLchar> null_terminated_string(StringView string)
    {
        Vector<GLchar> result;
        result.ensure_capacity(string.length() + 1);
        for (auto c : string.bytes())
            result.append(c);
        result.append('\0');
        return result;
    }

    GLenum get_error_value();
    void set_error(GLenum error);

    // UNPACK_FLIP_Y_WEBGL of type boolean
    //      If set, then during any subsequent calls to texImage2D or texSubImage2D, the source data is flipped along
    //      the vertical axis, so that conceptually the last row is the first one transferred. The initial value is false.
    //      Any non-zero value is interpreted as true.
    bool m_unpack_flip_y { false };

    // UNPACK_PREMULTIPLY_ALPHA_WEBGL of type boolean
    //      If set, then during any subsequent calls to texImage2D or texSubImage2D, the alpha channel of the source data,
    //      if present, is multiplied into the color channels during the data transfer. The initial value is false.
    //      Any non-zero value is interpreted as true.
    bool m_unpack_premultiply_alpha { false };

    // UNPACK_COLORSPACE_CONVERSION_WEBGL of type unsigned long
    //      If set to BROWSER_DEFAULT_WEBGL, then the browser's default colorspace conversion (e.g. converting a display-p3
    //      image to srgb) is applied during subsequent texture data upload calls (e.g. texImage2D and texSubImage2D) that
    //      take an argument of TexImageSource. The precise conversions may be specific to both the browser and file type.
    //      If set to NONE, no colorspace conversion is applied, other than conversion to RGBA. (For example, a rec709 YUV
    //      video is still converted to rec709 RGB data, but not then converted to e.g. srgb RGB data) The initial value is
    //      BROWSER_DEFAULT_WEBGL.
    GLenum m_unpack_colorspace_conversion { BROWSER_DEFAULT_WEBGL };

private:
    GLenum m_error { 0 };
};

}
