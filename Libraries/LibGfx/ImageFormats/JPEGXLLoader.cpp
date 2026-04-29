/*
 * Copyright (c) 2023-2024, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <RustFFI.h>

namespace Gfx {

class JPEGXLLoadingContext {
    AK_MAKE_NONCOPYABLE(JPEGXLLoadingContext);
    AK_MAKE_NONMOVABLE(JPEGXLLoadingContext);

public:
    enum class State : u8 {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
    };

    explicit JPEGXLLoadingContext(FFI::JxlRsDecoder* decoder)
        : m_decoder(decoder)
    {
    }

    ~JPEGXLLoadingContext()
    {
        if (m_decoder)
            FFI::jxl_rs_decoder_destroy(m_decoder);
    }

    static ErrorOr<NonnullOwnPtr<JPEGXLLoadingContext>> create(ReadonlyBytes data)
    {
        auto* decoder = FFI::jxl_rs_decoder_create(data.data(), data.size());
        if (!decoder)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Failed to create decoder.");

        auto context = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLLoadingContext(decoder)));

        if (FFI::jxl_rs_decoder_basic_info(decoder, &context->m_basic_info) != FFI::JxlRsStatus::Success)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Failed to read basic info.");

        if (context->m_basic_info.width == 0 || context->m_basic_info.height == 0)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Invalid image dimensions.");

        context->m_state = State::HeaderDecoded;
        return context;
    }

    ErrorOr<ImageFrameDescriptor> frame(size_t index)
    {
        if (m_state == State::Error)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoding failed.");

        auto count = FFI::jxl_rs_decoder_frame_count(m_decoder);
        if (index >= count)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Invalid frame index.");

        if (m_frame_cache.size() < count)
            TRY(m_frame_cache.try_resize(count));

        if (auto& cached = m_frame_cache[index]; cached.has_value())
            return *cached;

        IntSize size { static_cast<int>(m_basic_info.width), static_cast<int>(m_basic_info.height) };
        auto alpha_type = m_basic_info.alpha_premultiplied ? AlphaType::Premultiplied : AlphaType::Unpremultiplied;
        bool premultiply = alpha_type == AlphaType::Premultiplied;

        auto bitmap = TRY(Bitmap::create(BitmapFormat::RGBA8888, alpha_type, size));
        auto* pixels = reinterpret_cast<u8*>(bitmap->scanline(0));
        auto row_stride = static_cast<size_t>(bitmap->pitch());
        auto buffer_size = bitmap->size_in_bytes();

        auto status = FFI::jxl_rs_decoder_decode_frame(
            m_decoder,
            index,
            premultiply,
            pixels,
            buffer_size,
            m_basic_info.width,
            m_basic_info.height,
            row_stride);
        if (status != FFI::JxlRsStatus::Success) {
            m_state = State::Error;
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Failed to decode frame.");
        }

        ImageFrameDescriptor descriptor { bitmap, frame_duration(index) };
        m_frame_cache[index] = descriptor;
        return descriptor;
    }

    int frame_duration(size_t index) const
    {
        // Read durations from the Rust-side scan cache so callers (notably
        // the streaming animation path in the ImageDecoder service) can
        // collect timings cheaply *before* any pixel decode happens. Falling
        // back to per-descriptor data would return 0 for every frame that
        // hasn't been decoded yet, producing frantic playback.
        if (index >= FFI::jxl_rs_decoder_frame_count(m_decoder))
            return 0;
        auto duration_ms = FFI::jxl_rs_decoder_frame_duration_ms(m_decoder, index);
        if (!isfinite(duration_ms) || duration_ms <= 0.0)
            return 0;
        return static_cast<int>(duration_ms);
    }

    ErrorOr<Optional<ReadonlyBytes>> icc_data()
    {
        if (m_icc_loaded)
            return m_icc_data.is_empty() ? Optional<ReadonlyBytes> {} : Optional<ReadonlyBytes> { m_icc_data.span() };

        u8 const* icc_ptr = nullptr;
        size_t icc_len = 0;
        auto status = FFI::jxl_rs_decoder_icc_profile(m_decoder, &icc_ptr, &icc_len);
        m_icc_loaded = true;
        if (status != FFI::JxlRsStatus::Success)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Failed to read ICC profile.");

        if (icc_ptr && icc_len > 0)
            m_icc_data = TRY(ByteBuffer::copy(icc_ptr, icc_len));

        if (m_icc_data.is_empty())
            return OptionalNone {};
        return ReadonlyBytes { m_icc_data.span() };
    }

    State state() const { return m_state; }
    IntSize size() const { return { static_cast<int>(m_basic_info.width), static_cast<int>(m_basic_info.height) }; }
    bool is_animated() const { return m_basic_info.have_animation; }
    u32 loop_count() const { return m_basic_info.animation_loop_count; }

    size_t frame_count() const
    {
        return FFI::jxl_rs_decoder_frame_count(m_decoder);
    }

private:
    FFI::JxlRsDecoder* m_decoder { nullptr };
    State m_state { State::NotDecoded };
    FFI::JxlRsBasicInfo m_basic_info {};

    // Lazily populated cache of decoded frames. The streaming animation path
    // requests frames in batches via `frame()` and we'd rather decode them
    // one at a time than eagerly walk the whole codestream up front.
    Vector<Optional<ImageFrameDescriptor>> m_frame_cache;

    ByteBuffer m_icc_data;
    bool m_icc_loaded { false };
};

JPEGXLImageDecoderPlugin::JPEGXLImageDecoderPlugin(OwnPtr<JPEGXLLoadingContext> context)
    : m_context(move(context))
{
}

JPEGXLImageDecoderPlugin::~JPEGXLImageDecoderPlugin() = default;

IntSize JPEGXLImageDecoderPlugin::size()
{
    return m_context->size();
}

bool JPEGXLImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return FFI::jxl_rs_signature_check(data.data(), data.size());
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPEGXLImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto context = TRY(JPEGXLLoadingContext::create(data));
    return adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLImageDecoderPlugin(move(context)));
}

bool JPEGXLImageDecoderPlugin::is_animated()
{
    return m_context->is_animated();
}

size_t JPEGXLImageDecoderPlugin::loop_count()
{
    return m_context->loop_count();
}

size_t JPEGXLImageDecoderPlugin::frame_count()
{
    return m_context->frame_count();
}

size_t JPEGXLImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> JPEGXLImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    return m_context->frame(index);
}

int JPEGXLImageDecoderPlugin::frame_duration(size_t index)
{
    return m_context->frame_duration(index);
}

ErrorOr<Optional<ReadonlyBytes>> JPEGXLImageDecoderPlugin::icc_data()
{
    return m_context->icc_data();
}

}
