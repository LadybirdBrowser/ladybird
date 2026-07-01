/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Checked.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/PlatformObject.h>
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
using TexImageSource = Variant<GC::Ref<HTML::ImageBitmap>, GC::Ref<HTML::ImageData>, GC::Ref<HTML::HTMLImageElement>, GC::Ref<HTML::HTMLCanvasElement>, GC::Ref<HTML::OffscreenCanvas>, GC::Ref<HTML::HTMLVideoElement>>;

class WebGLRenderingContextBase : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGLRenderingContextBase, Bindings::PlatformObject);

public:
    using Float32List = Variant<GC::Ref<JS::Float32Array>, Vector<float>>;
    using Int32List = Variant<GC::Ref<JS::Int32Array>, Vector<WebIDL::Long>>;
    using Uint32List = Variant<GC::Ref<JS::Uint32Array>, Vector<WebIDL::UnsignedLong>>;

    virtual WebGLContextProxy& context() = 0;
    virtual GC::Ref<HTML::HTMLCanvasElement> canvas_for_binding() const = 0;

    u64 context_generation() const { return m_context_generation; }

    bool is_context_lost() const;

    // https://registry.khronos.org/webgl/specs/latest/1.0/#CONTEXT_LOST
    // The compositor process (and with it the GL state) went away: set the context lost
    // flag, stop talking to the dead host, and fire webglcontextlost at the canvas.
    void lose_context_from_compositor_loss();

    // The compositor came back. If the page asked for restoration (called preventDefault on
    // webglcontextlost), build a fresh remote context and fire webglcontextrestored so the
    // page can re-create its now-lost GL resources.
    void restore_context_after_compositor_reconnect();

    bool xr_compatible() const { return m_xr_compatible; }
    void set_xr_compatible(bool xr_compatible) { m_xr_compatible = xr_compatible; }

    // https://immersive-web.github.io/webxr/#dom-webglrenderingcontextbase-makexrcompatible
    GC::Ref<WebIDL::Promise> make_xr_compatible();

    Optional<Vector<String>> get_supported_extensions();
    JS::Object* get_extension(String const& name);

    void enable_compressed_texture_format(WebIDL::UnsignedLong format);

protected:
    WebGLRenderingContextBase(JS::Realm&);

    virtual void visit_edges(Cell::Visitor&) override;

    // Builds a fresh transport and host context against the reconnected compositor and
    // rebinds the proxy to it. Returns false if no compositor is available. WebGL1/2
    // implement it because they own the version and the actual context parameters.
    virtual bool reestablish_remote_context() = 0;
    virtual void reset_client_side_webgl_state() = 0;

    // FIXME: Make this and any another instance of extension names a FlyString, similarly to HTML::TagNames
    bool extension_enabled(StringView extension) const;
    ReadonlySpan<WebIDL::UnsignedLong> enabled_compressed_texture_formats() const;

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

    static ErrorOr<ByteBuffer> copy_buffer_source_to_byte_buffer(WebIDL::BufferSource src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        auto array_buffer = src_data.viewed_array_buffer();
        if (!array_buffer || array_buffer->is_detached()) [[unlikely]]
            return Error::from_errno(EINVAL);

        if (src_data.is_out_of_bounds()) {
            if (src_offset != 0 || src_length_override != 0) [[unlikely]]
                return Error::from_errno(EINVAL);
            return ByteBuffer::create_uninitialized(0);
        }

        auto element_size = src_data.element_size();
        Checked<size_t> byte_offset_in_view = static_cast<size_t>(src_offset);
        byte_offset_in_view *= element_size;
        if (byte_offset_in_view.has_overflow() || byte_offset_in_view.value() > src_data.byte_length()) [[unlikely]]
            return Error::from_errno(EINVAL);

        auto byte_length = src_data.byte_length() - byte_offset_in_view.value();
        if (src_length_override != 0) {
            Checked<size_t> requested_byte_length = static_cast<size_t>(src_length_override);
            requested_byte_length *= element_size;
            if (requested_byte_length.has_overflow() || requested_byte_length.value() > byte_length) [[unlikely]]
                return Error::from_errno(EINVAL);
            byte_length = requested_byte_length.value();
        }

        Checked<size_t> byte_offset_in_buffer = src_data.byte_offset();
        byte_offset_in_buffer += byte_offset_in_view.value();
        if (byte_offset_in_buffer.has_overflow()) [[unlikely]]
            return Error::from_errno(EINVAL);

        if (byte_offset_in_buffer.value() > array_buffer->byte_length()) [[unlikely]]
            return Error::from_errno(EINVAL);
        if (byte_length > array_buffer->byte_length() - byte_offset_in_buffer.value()) [[unlikely]]
            return Error::from_errno(EINVAL);

        return array_buffer->copy_to_byte_buffer(byte_offset_in_buffer.value(), byte_length);
    }

    template<typename T>
    class SpanWithStorage {
    public:
        explicit SpanWithStorage(Span<T> span)
            : m_span(span)
        {
        }

        explicit SpanWithStorage(ByteBuffer storage)
            : m_storage(move(storage))
            , m_has_storage(true)
            , m_span(m_storage.bytes().template reinterpret<T>())
        {
        }

        SpanWithStorage(SpanWithStorage const&) = delete;
        SpanWithStorage& operator=(SpanWithStorage const&) = delete;

        SpanWithStorage(SpanWithStorage&& other)
            : m_storage(move(other.m_storage))
            , m_has_storage(exchange(other.m_has_storage, false))
            , m_span(m_has_storage ? m_storage.bytes().template reinterpret<T>() : other.m_span)
        {
        }

        SpanWithStorage& operator=(SpanWithStorage&& other)
        {
            if (this != &other) {
                m_storage = move(other.m_storage);
                m_has_storage = exchange(other.m_has_storage, false);
                m_span = m_has_storage ? m_storage.bytes().template reinterpret<T>() : other.m_span;
            }
            return *this;
        }

        size_t size() const { return m_span.size(); }
        T* data() { return m_span.data(); }
        T const* data() const { return m_span.data(); }

    private:
        ByteBuffer m_storage;
        bool m_has_storage { false };
        Span<T> m_span;
    };

    template<typename T>
    static ErrorOr<SpanWithStorage<T>> span_from_typed_array(JS::TypedArrayBase& typed_array, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        auto record = JS::make_typed_array_with_buffer_witness_record(typed_array, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_typed_array_out_of_bounds(record)) [[unlikely]] {
            if (src_offset == 0 && src_length_override == 0)
                return SpanWithStorage<T> { Span<T> {} };
            return Error::from_errno(EINVAL);
        }

        auto length = JS::typed_array_length(record);
        Checked<size_t> end = static_cast<size_t>(src_offset);
        end += src_length_override;
        if (end.has_overflow() || end.value() > length) [[unlikely]]
            return Error::from_errno(EINVAL);

        auto elements_to_copy = src_length_override == 0 ? length - static_cast<size_t>(src_offset) : static_cast<size_t>(src_length_override);
        Checked<size_t> byte_offset = static_cast<size_t>(src_offset);
        byte_offset *= sizeof(T);
        byte_offset += typed_array.byte_offset();

        Checked<size_t> byte_length = elements_to_copy;
        byte_length *= sizeof(T);
        if (byte_offset.has_overflow() || byte_length.has_overflow()) [[unlikely]]
            return Error::from_errno(EINVAL);

        auto bytes = TRY(typed_array.viewed_array_buffer()->copy_to_byte_buffer(byte_offset.value(), byte_length.value()));
        return SpanWithStorage<T> { move(bytes) };
    }

    static ErrorOr<SpanWithStorage<float>> span_from_float32_list(Float32List& float32_list, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        if (float32_list.has<Vector<float>>()) {
            auto& vector = float32_list.get<Vector<float>>();
            return SpanWithStorage<float> { TRY(get_offset_span(vector.span(), src_offset, src_length_override)) };
        }
        auto& buffer = float32_list.get<GC::Ref<JS::Float32Array>>();
        return span_from_typed_array<float>(*buffer, src_offset, src_length_override);
    }

    static ErrorOr<SpanWithStorage<int>> span_from_int32_list(Int32List& int32_list, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        if (int32_list.has<Vector<int>>()) {
            auto& vector = int32_list.get<Vector<int>>();
            return SpanWithStorage<int> { TRY(get_offset_span(vector.span(), src_offset, src_length_override)) };
        }
        auto& buffer = int32_list.get<GC::Ref<JS::Int32Array>>();
        return span_from_typed_array<int>(*buffer, src_offset, src_length_override);
    }

    static ErrorOr<SpanWithStorage<u32>> span_from_uint32_list(Uint32List& uint32_list, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override = 0)
    {
        if (uint32_list.has<Vector<u32>>()) {
            auto& vector = uint32_list.get<Vector<u32>>();
            return SpanWithStorage<u32> { TRY(get_offset_span(vector.span(), src_offset, src_length_override)) };
        }
        auto& buffer = uint32_list.get<GC::Ref<JS::Uint32Array>>();
        return span_from_typed_array<u32>(*buffer, src_offset, src_length_override);
    }

    struct TexImageSourceFrame {
        Gfx::DecodedImageFrame frame;
        bool flip_y { false };
        bool premultiply_alpha { false };
    };
    Optional<TexImageSourceFrame> read_texture_image_source(TexImageSource const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type);

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
    void reset_context_state_after_loss();

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

    // https://registry.khronos.org/webgl/specs/latest/2.0/#webgl-context-lost-flag
    // Each WebGLRenderingContext and WebGL2RenderingContext has a webgl context lost flag, which is initially unset.
    bool m_context_lost { false };
    bool m_context_restore_requested { false };

    // https://immersive-web.github.io/webxr/#xr-compatible
    bool m_xr_compatible { false };
    u64 m_context_generation { 0 };

    Vector<WebIDL::UnsignedLong> m_enabled_compressed_texture_formats;

    // Extensions
    // "Multiple calls to getExtension with the same extension string, taking into account case-insensitive comparison, must return the same object as long as the extension is enabled."
    HashMap<String, GC::Ref<JS::Object>, AK::ASCIICaseInsensitiveStringTraits> m_enabled_extensions;
};

}
