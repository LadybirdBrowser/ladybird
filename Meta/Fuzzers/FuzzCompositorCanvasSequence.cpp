/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include "Fuzzing/Dispatch.h"
#include <AK/Array.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Painting/Canvas2DCommandStream.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    Core::EventLoop loop;
    auto state = Compositor::CompositorState::create(nullptr, false);
    using Client = Compositor::ConnectionFromWebContent;
    using namespace Messages::CompositorWebContentServer;
    Array<RefPtr<Client>, 2> clients;
    Array<OwnPtr<IPC::Transport>, 2> peers;
    Array<Optional<Web::Painting::CanvasId>, 2> canvases;
    Array<Gfx::Color, 2> expected { Gfx::Color::Black, Gfx::Color::Black };
    for (size_t owner = 0; owner < 2; ++owner) {
        auto pair = MUST(IPC::Transport::create_paired());
        peers[owner] = MUST(pair.remote_handle.create_transport());
        clients[owner] = Client::construct(move(pair.local), state, owner + 1);
    }
    auto create = [&](size_t owner) {
        auto response = Fuzzing::dispatch(*clients[owner], CreateCanvas2dContext { { 8, 8 }, false });
        VERIFY(response.success());
        canvases[owner] = response.canvas_id();
        expected[owner] = Gfx::Color::Black;
    };
    create(0);
    create(1);
    Fuzzing::BoundedInput input({ data, size });
    for (size_t step = 0; step < 48 && !input.remaining().is_empty(); ++step) {
        size_t actor = input.byte() % 2;
        size_t owner = input.byte() % 2;
        auto operation = input.byte() % 4;
        if (!canvases[owner].has_value())
            create(owner);
        auto id = *canvases[owner];
        switch (operation) {
        case 0: {
            auto red = input.byte();
            auto green = input.byte();
            auto blue = input.byte();
            Gfx::Color color(red, green, blue);
            Gfx::CanvasCommandList commands;
            commands.append(Gfx::CanvasCommands::FillRect { { 0, 0, 8, 8 }, color });
            Vector<Web::Painting::Canvas2DCommandStreamSegment> segments;
            segments.append({ id, move(commands), true });
            Fuzzing::dispatch(*clients[actor], UpdateCanvas2dStream { move(segments), {} });
            if (actor == owner)
                expected[owner] = color;
            break;
        }
        case 1: {
            auto reply = Fuzzing::dispatch(*clients[actor], GetCanvasPixels { id, { 0, 0, 8, 8 } });
            VERIFY(reply.pixels().is_valid() == (actor == owner));
            break;
        }
        case 2:
            Fuzzing::dispatch(*clients[actor], DestroyCanvasContext { id });
            if (actor == owner)
                canvases[owner].clear();
            break;
        case 3: {
            // Missing IDs and clipped/empty rectangles take the real admission path.
            auto response = Fuzzing::dispatch(*clients[actor], GetCanvasPixels { Web::Painting::CanvasId { 0 }, { -8, -8, 0, 0 } });
            VERIFY(!response.pixels().is_valid());
            break;
        }
        }
        for (size_t victim = 0; victim < 2; ++victim) {
            if (!canvases[victim].has_value())
                continue;
            auto response = Fuzzing::dispatch(*clients[victim], GetCanvasPixels { *canvases[victim], { 0, 0, 8, 8 } });
            VERIFY(response.pixels().is_valid());
            auto const* bitmap = response.pixels().bitmap();
            VERIFY(bitmap->size() == Gfx::IntSize(8, 8));
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x)
                    VERIFY(bitmap->get_pixel(x, y) == expected[victim]);
            }
            // Cross-owner destruction must preserve the registry as well as pixels.
            VERIFY(state->canvas_surface_registry().canvas_surface(*canvases[victim]));
        }
    }
    for (auto& client : clients) {
        client->shutdown();
        client = nullptr;
    }
    for (auto const& canvas : canvases) {
        if (canvas.has_value())
            VERIFY(!state->canvas_surface_registry().canvas_surface(*canvas));
    }
    for (auto& peer : peers)
        peer = nullptr;
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    return 0;
}
