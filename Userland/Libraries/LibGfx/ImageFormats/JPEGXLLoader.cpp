/*
 * Copyright (c) 2023-2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Error.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <jxl/decode.h>

namespace Gfx {

class JPEGXLLoadingContext {
public:
    JPEGXLLoadingContext(JxlDecoder* decoder)
        : m_decoder(decoder)
    {
    }

    ~JPEGXLLoadingContext()
    {
        JxlDecoderDestroy(m_decoder);
    }

    ErrorOr<void> decode_image_header()
    {
        return run_state_machine_until(State::HeaderDecoded);
    }

    ErrorOr<void> decode_image()
    {
        return run_state_machine_until(State::FrameDecoded);
    }

    enum class State : u8 {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
        FrameDecoded,
    };

    State state() const
    {
        return m_state;
    }

    IntSize size() const
    {
        return m_size;
    }

    RefPtr<Bitmap> bitmap() const
    {
        return m_bitmap;
    }

private:
    ErrorOr<void> run_state_machine_until(State requested_state)
    {
        for (;;) {
            auto const status = JxlDecoderProcessInput(m_decoder);

            if (status == JXL_DEC_ERROR)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoder is corrupted.");
            if (status == JXL_DEC_NEED_MORE_INPUT)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoder need more input.");

            if (status == JXL_DEC_BASIC_INFO) {
                TRY(decode_image_header_impl());
                if (requested_state <= State::HeaderDecoded)
                    return {};
            }

            if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                TRY(set_output_buffer());
                continue;
            }

            if (status == JXL_DEC_FULL_IMAGE) {
                // Called once per frame, let's return for now
                return {};
            }

            if (status == JXL_DEC_SUCCESS)
                return {};

            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unknown event.");
        }
    }

    ErrorOr<void> decode_image_header_impl()
    {
        JxlBasicInfo info;

        if (auto res = JxlDecoderGetBasicInfo(m_decoder, &info); res != JXL_DEC_SUCCESS)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to decode basic information.");

        m_size = { info.xsize, info.ysize };

        m_state = State::HeaderDecoded;
        return {};
    }

    ErrorOr<void> set_output_buffer()
    {
        auto result = [this]() -> ErrorOr<void> {
            if (JxlDecoderProcessInput(m_decoder) != JXL_DEC_NEED_IMAGE_OUT_BUFFER)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoder is in an unexpected state.");

            m_bitmap = TRY(Bitmap::create(Gfx::BitmapFormat::RGBA8888, m_size));

            JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };

            size_t needed_size = 0;
            JxlDecoderImageOutBufferSize(m_decoder, &format, &needed_size);

            if (needed_size != m_bitmap->size_in_bytes())
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Expected bitmap size is wrong.");

            if (auto res = JxlDecoderSetImageOutBuffer(m_decoder, &format, m_bitmap->begin(), m_bitmap->size_in_bytes());
                res != JXL_DEC_SUCCESS)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to decode frame.");

            return {};
        }();

        m_state = result.is_error() ? State::Error : State::FrameDecoded;
        return result;
    }

    State m_state { State::NotDecoded };

    JxlDecoder* m_decoder;

    IntSize m_size;
    RefPtr<Bitmap> m_bitmap;
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
    auto signature = JxlSignatureCheck(data.data(), data.size());
    return signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER;
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPEGXLImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto* decoder = JxlDecoderCreate(nullptr);
    if (!decoder)
        return Error::from_errno(ENOMEM);

    if (auto res = JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE); res == JXL_DEC_ERROR)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to subscribe to events.");

    if (auto res = JxlDecoderSetInput(decoder, data.data(), data.size()); res == JXL_DEC_ERROR)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to set decoder input.");

    // Tell the decoder that it won't receive more data for the image.
    JxlDecoderCloseInput(decoder);

    auto context = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLLoadingContext(decoder)));
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLImageDecoderPlugin(move(context))));

    TRY(plugin->m_context->decode_image_header());
    return plugin;
}

bool JPEGXLImageDecoderPlugin::is_animated()
{
    return false;
}

size_t JPEGXLImageDecoderPlugin::loop_count()
{
    return 0;
}

size_t JPEGXLImageDecoderPlugin::frame_count()
{
    return 1;
}

size_t JPEGXLImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> JPEGXLImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Invalid frame index.");

    if (m_context->state() == JPEGXLLoadingContext::State::Error)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoding failed.");

    if (m_context->state() < JPEGXLLoadingContext::State::FrameDecoded)
        TRY(m_context->decode_image());

    return ImageFrameDescriptor { m_context->bitmap(), 0 };
}

ErrorOr<Optional<ReadonlyBytes>> JPEGXLImageDecoderPlugin::icc_data()
{
    return OptionalNone {};
}
}
