/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapSequence.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibTest/TestCase.h>

namespace {

class ImageDecoderStub final : public IPC::Stub {
public:
    virtual u32 magic() const override { return ImageDecoderServerEndpoint::static_magic(); }
    virtual ByteString name() const override { return "Test ImageDecoder"; }

    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle(NonnullOwnPtr<IPC::Message> message) override
    {
        if (message->message_id() == Messages::ImageDecoderServer::DecodeImage::static_message_id())
            m_request_id = static_cast<Messages::ImageDecoderServer::DecodeImage&>(*message).request_id();
        return OwnPtr<IPC::MessageBuffer> {};
    }

    Optional<i64> request_id() const { return m_request_id; }

private:
    Optional<i64> m_request_id;
};

ErrorOr<IPC::MessageBuffer> encode_empty_image_response(i64 request_id)
{
    auto backing = TRY(Core::AnonymousBuffer::create_with_size(1));
    Vector<Optional<Gfx::BitmapMetadata>> metadata;
    TRY(metadata.try_append(Gfx::BitmapMetadata {
        .format = Gfx::BitmapFormat::RGBA8888,
        .alpha_type = Gfx::AlphaType::Premultiplied,
        .size = { 1, 0 },
        .size_in_bytes = backing.size(),
    }));

    IPC::MessageBuffer response;
    IPC::Encoder encoder { response };
    TRY(encoder.encode(Messages::ImageDecoderClient::DidDecodeImage::ENDPOINT_MAGIC));
    TRY(encoder.encode(Messages::ImageDecoderClient::DidDecodeImage::static_message_id()));
    TRY(encoder.encode(request_id));
    TRY(encoder.encode(false));
    TRY(encoder.encode(static_cast<u32>(0)));
    TRY(encoder.encode(metadata));
    TRY(encoder.encode(backing.size()));
    TRY(encoder.encode(backing));
    Array<u32, 1> durations { 1 };
    TRY(encoder.encode(durations.span()));
    TRY(encoder.encode(Gfx::FloatPoint { 1, 1 }));
    TRY(encoder.encode(Gfx::ColorSpace {}));
    TRY(encoder.encode(static_cast<i64>(0)));
    return response;
}

}

TEST_CASE(ipc_decode_rejects_undersized_single_frame_backing)
{
    // Issue #10036: A single-frame BitmapSequence whose metadata describes a 50000x10 (2,000,000-byte) bitmap but ships
    // only a 4096-byte backing buffer. Decoding must fail rather than adopt the undersized buffer (and write OOB).
    constexpr size_t backing_size = 4096;
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(backing_size));

    Gfx::BitmapMetadata metadata {
        .format = Gfx::BitmapFormat::BGRA8888,
        .alpha_type = Gfx::AlphaType::Premultiplied,
        .size = Gfx::IntSize { 50000, 10 },
        .size_in_bytes = backing_size,
    };

    Vector<Optional<Gfx::BitmapMetadata>> metadata_list;
    metadata_list.append(metadata);

    // Hand-encode the wire fields in the order IPC::encode(BitmapSequence) uses. The message is internally consistent,
    // yet inconsistent with the claimed bitmap geometry — which is exactly what the decoder must catch.
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(metadata_list));
    MUST(encoder.encode(static_cast<size_t>(backing_size)));
    MUST(encoder.encode(buffer));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };

    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message_buffer.take_attachments())
        attachments.enqueue(move(attachment));

    IPC::Decoder decoder { stream, attachments };
    auto result = IPC::decode<Gfx::BitmapSequence>(decoder);
    EXPECT(result.is_error());
}

TEST_CASE(create_with_anonymous_buffer_rejects_undersized_buffer)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(4096));
    auto result = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, move(buffer), Gfx::IntSize { 50000, 10 });
    EXPECT(result.is_error());
}

TEST_CASE(create_with_anonymous_buffer_accepts_correctly_sized_buffer)
{
    Gfx::IntSize size { 64, 64 };
    auto required = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(size.width(), Gfx::BitmapFormat::BGRA8888), size.height());
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(required));
    auto bitmap = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, move(buffer), size);
    EXPECT(!bitmap.is_error());
}

TEST_CASE(create_with_raw_data_rejects_undersized_data)
{
    Array<u8, 16> tiny {};
    auto result = Gfx::Bitmap::create_with_raw_data(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, tiny, Gfx::IntSize { 50000, 10 });
    EXPECT(result.is_error());
}

TEST_CASE(image_decoder_client_rejects_empty_bitmap_geometry)
{
    Core::EventLoop loop;
    auto pair = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto peer_transport = TRY_OR_FAIL(pair.remote_handle.create_transport());
    ImageDecoderStub stub;
    auto peer = adopt_ref(*new IPC::Connection<ImageDecoderServerEndpoint, ImageDecoderClientEndpoint>(stub, move(peer_transport)));
    auto client = adopt_ref(*new ImageDecoderClient::Client(move(pair.local)));

    bool resolved = false;
    bool rejected = false;
    u8 encoded_byte = 0;
    [[maybe_unused]] auto promise = client->decode_image({ &encoded_byte, 1 }, [&](auto&) -> ErrorOr<void> {
            resolved = true;
            return {}; }, [&](auto&) { rejected = true; });

    client->transport().flush();
    peer->dispatch_pending_messages();
    EXPECT(stub.request_id().has_value());

    auto response = TRY_OR_FAIL(encode_empty_image_response(stub.request_id().value()));
    auto data = response.take_data();
    auto attachments = response.take_attachments();
    TRY_OR_FAIL(peer->transport().post_message(move(data), attachments));
    peer->transport().flush();
    client->dispatch_pending_messages();

    EXPECT(rejected);
    EXPECT(!resolved);
}
