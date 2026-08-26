/*
 * Copyright (c) 2023-2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibImageDecoders/RustFFI.h>

namespace Gfx {

class JPEGXLLoadingContext {
    AK_MAKE_NONCOPYABLE(JPEGXLLoadingContext);
    AK_MAKE_NONMOVABLE(JPEGXLLoadingContext);

public:
    JPEGXLLoadingContext(ImageDecoders::FFI::JPEGXLDecoder* decoder, ImageDecoders::FFI::JPEGXLImageInfo const& info, ReadonlyBytes data, IntSize size)
        : m_decoder(decoder)
        , m_data(data)
        , m_size(size)
        , m_alpha_type(info.alpha_premultiplied ? Gfx::AlphaType::Premultiplied : Gfx::AlphaType::Unpremultiplied)
        , m_animated(info.is_animated)
        , m_loop_count(info.loop_count)
    {
    }

    ~JPEGXLLoadingContext()
    {
        ImageDecoders::FFI::jpegxl_rust_decoder_free(m_decoder);
    }

    ErrorOr<void> decode_image()
    {
        if (!ImageDecoders::FFI::jpegxl_rust_decode(m_decoder, m_data.data(), m_data.size(), this, get_frame_buffer, frame_decoded)) {
            m_state = State::Error;
            m_pending_bitmap = nullptr;
            m_frame_descriptors.clear();
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoding failed.");
        }

        if (m_pending_bitmap) {
            m_state = State::Error;
            m_pending_bitmap = nullptr;
            m_frame_descriptors.clear();
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoding failed.");
        }

        m_state = State::FrameDecoded;
        return {};
    }

    enum class State : u8 {
        HeaderDecoded,
        FrameDecoded,
        Error,
    };

    State state() const { return m_state; }
    IntSize size() const { return m_size; }

    Vector<ImageFrameDescriptor> const& frame_descriptors() const { return m_frame_descriptors; }

    bool is_animated() const { return m_animated; }
    u32 loop_count() const { return m_loop_count; }

private:
    static bool get_frame_buffer(void* context, u32 duration_ms, u8** buffer, size_t* stride)
    {
        auto& self = *static_cast<JPEGXLLoadingContext*>(context);
        if (self.m_pending_bitmap)
            return false;

        auto bitmap_or_error = Bitmap::create(Gfx::BitmapFormat::RGBA8888, self.m_alpha_type, self.m_size);
        if (bitmap_or_error.is_error())
            return false;

        self.m_pending_bitmap = bitmap_or_error.release_value();
        self.m_pending_frame_duration = AK::clamp_to<int>(duration_ms);
        *buffer = self.m_pending_bitmap->scanline_u8(0);
        *stride = self.m_pending_bitmap->pitch();

        return true;
    }

    static bool frame_decoded(void* context)
    {
        auto& self = *static_cast<JPEGXLLoadingContext*>(context);
        if (!self.m_pending_bitmap)
            return false;

        auto result = self.m_frame_descriptors.try_empend(self.m_pending_bitmap.release_nonnull(), self.m_pending_frame_duration);
        return !result.is_error();
    }

    ImageDecoders::FFI::JPEGXLDecoder* m_decoder { nullptr };
    State m_state { State::HeaderDecoded };

    ReadonlyBytes m_data;
    IntSize m_size;

    RefPtr<Bitmap> m_pending_bitmap;
    int m_pending_frame_duration { 0 };

    Vector<ImageFrameDescriptor> m_frame_descriptors;

    Gfx::AlphaType m_alpha_type { Gfx::AlphaType::Unpremultiplied };
    bool m_animated { false };
    u32 m_loop_count { 0 };
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
    return ImageDecoders::FFI::jpegxl_rust_sniff(data.data(), data.size());
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPEGXLImageDecoderPlugin::create(ReadonlyBytes data)
{
    ImageDecoders::FFI::JPEGXLImageInfo info {};
    auto* decoder = ImageDecoders::FFI::jpegxl_rust_decoder_new(data.data(), data.size(), &info);
    if (!decoder)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to decode basic information.");
    ArmedScopeGuard free_decoder = [decoder]() { ImageDecoders::FFI::jpegxl_rust_decoder_free(decoder); };

    if (info.width > static_cast<size_t>(NumericLimits<int>::max()) || info.height > static_cast<size_t>(NumericLimits<int>::max()))
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Image dimensions are too large.");

    IntSize size { static_cast<int>(info.width), static_cast<int>(info.height) };
    if (Bitmap::size_would_overflow(BitmapFormat::RGBA8888, size))
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Image dimensions are too large.");

    auto context = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLLoadingContext(decoder, info, data, size)));
    free_decoder.disarm();

    return TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLImageDecoderPlugin(move(context))));
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
    if (m_context->state() == JPEGXLLoadingContext::State::HeaderDecoded)
        (void)frame(0);
    return m_context->frame_descriptors().size();
}

ErrorOr<ImageFrameDescriptor> JPEGXLImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (m_context->state() == JPEGXLLoadingContext::State::Error)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoding failed.");

    if (m_context->state() == JPEGXLLoadingContext::State::HeaderDecoded)
        TRY(m_context->decode_image());

    if (index >= m_context->frame_descriptors().size())
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Invalid frame index requested.");
    return m_context->frame_descriptors()[index];
}

int JPEGXLImageDecoderPlugin::frame_duration(size_t index)
{
    if (index >= m_context->frame_descriptors().size())
        return 0;
    return m_context->frame_descriptors()[index].duration;
}

ErrorOr<Optional<ReadonlyBytes>> JPEGXLImageDecoderPlugin::icc_data()
{
    return OptionalNone {};
}

}
