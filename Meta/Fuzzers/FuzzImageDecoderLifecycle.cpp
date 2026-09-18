/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/AsyncSupport.h"
#include "Fuzzing/BoundedInput.h"
#include "Fuzzing/Dispatch.h"
#include <AK/Array.h>
#include <ImageDecoder/ConnectionFromClient.h>
#include <LibGfx/Bitmap.h>

namespace {

// Production ImageDecoder exits when its last client disconnects. One idle
// keeper and one event loop persist; all case clients/jobs must expire below.
struct Runtime {
    Core::EventLoop loop;
    OwnPtr<IPC::Transport> peer;
    RefPtr<ImageDecoder::ConnectionFromClient> keeper;

    Runtime()
    {
        auto pair = MUST(IPC::Transport::create_paired());
        peer = MUST(pair.remote_handle.create_transport());
        keeper = ImageDecoder::ConnectionFromClient::construct(move(pair.local));
    }
};

Core::AnonymousBuffer bitmap_input(u8 green)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(58));
    auto* bytes = buffer.data<u8>();
    __builtin_memset(bytes, 0, 58);
    bytes[0] = 'B';
    bytes[1] = 'M';
    bytes[2] = 58;
    bytes[10] = 54;
    bytes[14] = 40;
    bytes[18] = 1;
    bytes[22] = 1;
    bytes[26] = 1;
    bytes[28] = 24;
    bytes[34] = 4;
    bytes[54] = 51;
    bytes[55] = green;
    bytes[56] = 17;
    return buffer;
}

}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    static Runtime runtime;
    using Client = ImageDecoder::ConnectionFromClient;
    using namespace Messages::ImageDecoderServer;
    Array<RefPtr<Client>, 2> clients;
    Array<OwnPtr<IPC::Transport>, 2> peers;
    Array<WeakPtr<Client>, 2> weak;
    Array<bool, 2> control_done {};
    Array<Array<bool, 10>, 2> issued {};
    Array<Array<bool, 10>, 2> cancelled {};
    Array<Array<bool, 10>, 2> completed {};
    struct Session {
        i64 id;
        size_t frames;
        size_t replies { 0 };
        bool stopped { false };
    };
    Array<Vector<Session>, 2> sessions;
    for (size_t owner = 0; owner < 2; ++owner) {
        auto pair = MUST(IPC::Transport::create_paired());
        peers[owner] = MUST(pair.remote_handle.create_transport());
        clients[owner] = Client::construct(move(pair.local));
        weak[owner] = clients[owner]->make_weak_ptr<Client>();
        issued[owner][0] = true;
        Fuzzing::dispatch(*clients[owner], DecodeImage { bitmap_input(34 + owner), {}, "image/bmp", 0 });
    }
    auto drain = [&] {
        for (size_t owner = 0; owner < 2; ++owner) {
            Fuzzing::receive<ImageDecoderClientEndpoint>(*peers[owner], [&](IPC::Message& message) {
                using namespace Messages::ImageDecoderClient;
                if (message.message_id() == DidDecodeImage::static_message_id()) {
                    auto const& result = static_cast<DidDecodeImage const&>(message);
                    auto id = result.request_id();
                    VERIFY(id >= 0 && id < 10 && issued[owner][id] && !cancelled[owner][id] && !completed[owner][id]);
                    completed[owner][id] = true;
                    if (id == 0) {
                        VERIFY(result.bitmaps().bitmaps.size() == 1);
                        auto const& bitmap = result.bitmaps().bitmaps[0];
                        VERIFY(bitmap && bitmap->size() == Gfx::IntSize(1, 1));
                        VERIFY(bitmap->get_pixel(0, 0) == Gfx::Color(17, 34 + owner, 51));
                        control_done[owner] = true;
                    }
                    if (result.session_id() > 0)
                        sessions[owner].append({ result.session_id(), result.durations().size() });
                } else if (message.message_id() == DidFailToDecodeImage::static_message_id()) {
                    auto id = static_cast<DidFailToDecodeImage const&>(message).request_id();
                    VERIFY(id > 0 && id < 10 && issued[owner][id] && !cancelled[owner][id] && !completed[owner][id]);
                    completed[owner][id] = true;
                } else {
                    // Frame replies must name a session observed on this connection.
                    VERIFY(message.message_id() == DidDecodeAnimationFrames::static_message_id() || message.message_id() == DidFailAnimationDecode::static_message_id());
                    i64 id = message.message_id() == DidDecodeAnimationFrames::static_message_id()
                        ? static_cast<DidDecodeAnimationFrames const&>(message).session_id()
                        : static_cast<DidFailAnimationDecode const&>(message).session_id();
                    auto session = sessions[owner].find_if([&](auto const& candidate) { return candidate.id == id; });
                    VERIFY(session != sessions[owner].end() && !session->stopped);
                    ++session->replies;
                }
            });
        }
    };
    Fuzzing::pump_until(runtime.loop, [&] { return control_done[0] && control_done[1]; }, drain, "image positive controls");

    Fuzzing::BoundedInput input({ data, size });
    for (i64 id = 1; id <= 8 && !input.remaining().is_empty(); ++id) {
        size_t owner = input.byte() % 2;
        auto mode = input.byte();
        auto buffer = bitmap_input(input.byte());
        if (mode & 1) {
            auto raw = input.take(min(size_t { 1024 }, size_t { input.byte() } * 4));
            if (!raw.is_empty()) {
                buffer = MUST(Core::AnonymousBuffer::create_with_size(raw.size()));
                __builtin_memcpy(buffer.data<void>(), raw.data(), raw.size());
            }
        }
        issued[owner][id] = true;
        Fuzzing::dispatch(*clients[owner], DecodeImage { move(buffer), Gfx::IntSize { 16, 16 }, {}, id });
        if (mode & 2) {
            // No event-loop callback has run since enqueue: cancellation must
            // suppress this result, even if the worker already finished decoding.
            Fuzzing::dispatch(*clients[owner], CancelDecoding { id });
            cancelled[owner][id] = true;
        }
        if (mode & 4) {
            runtime.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            drain();
        }
        if ((mode & 16) && !cancelled[owner][id])
            Fuzzing::pump_until(runtime.loop, [&] { return completed[owner][id]; }, drain, "selected image/session decode");
        if (!sessions[owner].is_empty()) {
            auto session_index = sessions[owner].size() - 1;
            auto& session = sessions[owner][session_index];
            auto start = input.byte();
            auto count = static_cast<u32>(input.byte() % 5);
            auto old_replies = session.replies;
            Fuzzing::dispatch(*clients[owner], RequestAnimationFrames { session.id, start, count });
            if (mode & 8) {
                Fuzzing::dispatch(*clients[owner], StopAnimationDecode { session.id });
                session.stopped = true;
            } else if ((mode & 32) && !session.stopped && start < session.frames) {
                // drain() can append sessions and reallocate the vector while waiting.
                Fuzzing::pump_until(runtime.loop, [&] { return sessions[owner][session_index].replies > old_replies; }, drain, "animation frame reply");
            }
        }
    }
    for (auto& client : clients) {
        client->shutdown();
        client = nullptr;
    }
    // Workers capture a strong receiver reference and the persistent main loop.
    // Do not return while an earlier input could still use its encoded buffer.
    Fuzzing::pump_until(runtime.loop, [&] { return !weak[0] && !weak[1]; }, drain, "image worker/receiver teardown");
    for (auto& peer : peers)
        peer = nullptr;
    return 0;
}
