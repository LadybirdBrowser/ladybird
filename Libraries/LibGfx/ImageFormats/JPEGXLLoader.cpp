/*
 * Copyright (c) 2023-2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <jxl/decode.h>

namespace Gfx {

static constexpr size_t READ_BUFFER_SIZE = 4 * KiB;

class JPEGXLLoadingContext {
    AK_MAKE_NONCOPYABLE(JPEGXLLoadingContext);
    AK_MAKE_NONMOVABLE(JPEGXLLoadingContext);

public:
    JPEGXLLoadingContext(JxlDecoder* decoder, NonnullRefPtr<ImageDecoderStream> stream)
        : m_decoder(decoder)
        , m_stream(move(stream))
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

    Vector<ImageFrameDescriptor> const& frame_descriptors() const
    {
        return m_frame_descriptors;
    }

    bool is_animated() const
    {
        return m_animated;
    }

    u32 loop_count() const
    {
        return m_loop_count;
    }

    u32 frame_count() const
    {
        return m_frame_count;
    }

private:
    ErrorOr<JxlDecoderStatus> perform_operation_that_may_require_more_input(Function<JxlDecoderStatus()> operation)
    {
        for (;;) {
            auto status = operation();
            if (status != JXL_DEC_NEED_MORE_INPUT)
                return status;

            size_t unprocessed_bytes = JxlDecoderReleaseInput(m_decoder);
            TRY(m_stream->seek(-unprocessed_bytes, SeekMode::FromCurrentPosition));
            auto bytes = TRY(m_stream->read_some(m_read_buffer));

            if (!bytes.is_empty()) {
                status = JxlDecoderSetInput(m_decoder, bytes.data(), bytes.size());
                if (status == JXL_DEC_ERROR)
                    return status;
            } else {
                JxlDecoderCloseInput(m_decoder);
            }
        }
    }

    ErrorOr<void> run_state_machine_until(State requested_state)
    {
        Optional<u32> frame_duration;
        for (;;) {
            auto const status = TRY(perform_operation_that_may_require_more_input([this] {
                return JxlDecoderProcessInput(m_decoder);
            }));

            if (status == JXL_DEC_ERROR)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoder is corrupted.");
            if (status == JXL_DEC_NEED_MORE_INPUT)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoder need more input.");

            if (status == JXL_DEC_BASIC_INFO) {
                TRY(decode_image_header_impl());
                if (requested_state <= State::HeaderDecoded)
                    return {};
            }

            if (status == JXL_DEC_FRAME) {
                JxlFrameHeader header;

                auto const res = TRY(perform_operation_that_may_require_more_input([this, &header] {
                    return JxlDecoderGetFrameHeader(m_decoder, &header);
                }));

                if (res != JXL_DEC_SUCCESS)
                    return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to retrieve frame header.");

                frame_duration = header.duration;
                continue;
            }

            if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                if (!frame_duration.has_value())
                    return Error::from_string_literal("JPEGXLImageDecoderPlugin: No frame header was read.");

                TRY(set_output_buffer(*frame_duration));
                continue;
            }

            if (status == JXL_DEC_FULL_IMAGE) {
                m_frame_count++;
                continue;
            }

            if (status == JXL_DEC_SUCCESS) {
                if (m_state != State::Error)
                    m_state = State::FrameDecoded;
                return {};
            }

            warnln("JPEGXLImageDecoderPlugin: Unknown event.");
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unknown event.");
        }
    }

    ErrorOr<void> decode_image_header_impl()
    {
        JxlBasicInfo info;

        auto const res = TRY(perform_operation_that_may_require_more_input([this, &info] {
            return JxlDecoderGetBasicInfo(m_decoder, &info);
        }));

        if (res != JXL_DEC_SUCCESS)
            return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to decode basic information.");

        m_size = { info.xsize, info.ysize };

        m_animated = static_cast<bool>(info.have_animation);
        m_alpha_premultiplied = info.alpha_premultiplied ? Gfx::AlphaType::Premultiplied : Gfx::AlphaType::Unpremultiplied;

        if (m_animated)
            m_loop_count = info.animation.num_loops;

        m_state = State::HeaderDecoded;
        return {};
    }

    ErrorOr<void> set_output_buffer(u32 duration)
    {
        auto result = [this, duration]() -> ErrorOr<void> {
            auto res = TRY(perform_operation_that_may_require_more_input([this] {
                return JxlDecoderProcessInput(m_decoder);
            }));

            if (res != JXL_DEC_NEED_IMAGE_OUT_BUFFER)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoder is in an unexpected state.");

            auto bitmap = TRY(Bitmap::create(Gfx::BitmapFormat::RGBA8888, m_alpha_premultiplied, m_size));
            TRY(m_frame_descriptors.try_empend(bitmap, static_cast<int>(duration)));

            JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };

            size_t needed_size = 0;
            JxlDecoderImageOutBufferSize(m_decoder, &format, &needed_size);

            if (needed_size != bitmap->size_in_bytes())
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Expected bitmap size is wrong.");

            if (res = JxlDecoderSetImageOutBuffer(m_decoder, &format, bitmap->begin(), bitmap->size_in_bytes());
                res != JXL_DEC_SUCCESS)
                return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to decode frame.");

            return {};
        }();

        if (result.is_error()) {
            m_state = State::Error;
            warnln("{}", result.error());
        }

        return result;
    }

    State m_state { State::NotDecoded };

    JxlDecoder* m_decoder;
    NonnullRefPtr<ImageDecoderStream> m_stream;
    Array<u8, READ_BUFFER_SIZE> m_read_buffer;

    IntSize m_size;
    Vector<ImageFrameDescriptor> m_frame_descriptors;

    bool m_animated { false };
    Gfx::AlphaType m_alpha_premultiplied { Gfx::AlphaType::Premultiplied };
    u32 m_loop_count { 0 };
    u32 m_frame_count { 0 };
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

bool JPEGXLImageDecoderPlugin::sniff(NonnullRefPtr<ImageDecoderStream> stream)
{
    ByteBuffer buffer;
    JxlSignature signature = JXL_SIG_NOT_ENOUGH_BYTES;
    do {
        constexpr size_t SIGNATURE_READ_BUFFER_INCREMENT = 32;
        auto bytes = buffer.must_get_bytes_for_writing(SIGNATURE_READ_BUFFER_INCREMENT);
        MUST(stream->read_some(bytes));
        signature = JxlSignatureCheck(buffer.data(), buffer.size());
    } while (signature == JXL_SIG_NOT_ENOUGH_BYTES);
    return signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER;
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPEGXLImageDecoderPlugin::create(NonnullRefPtr<ImageDecoderStream> stream)
{
    auto* decoder = JxlDecoderCreate(nullptr);
    if (!decoder)
        return Error::from_errno(ENOMEM);

    auto const events = JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE;
    if (auto res = JxlDecoderSubscribeEvents(decoder, events); res == JXL_DEC_ERROR)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Unable to subscribe to events.");

    auto context = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLLoadingContext(decoder, move(stream))));
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEGXLImageDecoderPlugin(move(context))));

    TRY(plugin->m_context->decode_image_header());
    return plugin;
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
    // FIXME: There doesn't seem to be a way to have that information
    //        before decoding all the frames.
    if (m_context->frame_count() == 0)
        (void)frame(0);
    return m_context->frame_count();
}

size_t JPEGXLImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> JPEGXLImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (m_context->state() == JPEGXLLoadingContext::State::Error)
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Decoding failed.");

    if (m_context->state() < JPEGXLLoadingContext::State::FrameDecoded)
        TRY(m_context->decode_image());

    if (index >= m_context->frame_descriptors().size())
        return Error::from_string_literal("JPEGXLImageDecoderPlugin: Invalid frame index requested.");
    return m_context->frame_descriptors()[index];
}

ErrorOr<Optional<ReadonlyBytes>> JPEGXLImageDecoderPlugin::icc_data()
{
    return OptionalNone {};
}

}
