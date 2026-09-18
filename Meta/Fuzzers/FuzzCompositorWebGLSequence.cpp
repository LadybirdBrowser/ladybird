/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/AsyncSupport.h"
#include "Fuzzing/BoundedInput.h"
#include "Fuzzing/Dispatch.h"
#include <AK/Array.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/WebGL/WebGLCommandList.h>
#include <LibWeb/WebGL/WebGLSharedCommandBuffer.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    Core::EventLoop loop;
    auto state = Compositor::CompositorState::create(nullptr, false);
    using Client = Compositor::ConnectionFromWebContent;
    using namespace Web::WebGL;
    using namespace Messages::CompositorWebContentServer;
    Array<RefPtr<Client>, 2> clients;
    Array<OwnPtr<IPC::Transport>, 2> peers;
    Array<Web::Painting::CanvasId, 2> canvases { Web::Painting::CanvasId { 0 }, Web::Painting::CanvasId { 0 } };
    Array<WebGLSharedCommandBuffer, 2> buffers;
    Array<Gfx::Color, 2> colors { Gfx::Color::Black, Gfx::Color::Black };
    u64 sequence = 0;
    auto publish = [&](size_t owner, WebGLCommandList const& commands) {
        VERIFY(commands.size_in_bytes() <= buffers[owner].data_region().size());
        commands.bytes().copy_to(buffers[owner].data_region());
        Fuzzing::dispatch(*clients[owner], WebglCommandsFromSharedBuffer { canvases[owner], 0, commands.size_in_bytes(), ++sequence, {} });
        Fuzzing::dispatch(*clients[owner], WebglDrainCommandBuffer { canvases[owner] });
        VERIFY(clients[owner]->is_open());
        VERIFY(buffers[owner].executed_flush_sequence_number() == sequence);
    };
    auto fill = [&](size_t owner, u8 bits) {
        float r = (bits & 1) ? 1 : 0;
        float g = (bits & 2) ? 1 : 0;
        float b = (bits & 4) ? 1 : 0;
        WebGLCommandList commands;
        commands.append(Commands::ClearColor { r, g, b, 1 });
        commands.append(Commands::Clear { 0x4000 }); // GL_COLOR_BUFFER_BIT
        publish(owner, commands);
        colors[owner] = Gfx::Color(r ? 255 : 0, g ? 255 : 0, b ? 255 : 0);
    };
    auto create = [&](size_t owner) {
        if (clients[owner] && clients[owner]->is_open())
            clients[owner]->shutdown();
        peers[owner] = nullptr;
        auto pair = MUST(IPC::Transport::create_paired());
        peers[owner] = MUST(pair.remote_handle.create_transport());
        clients[owner] = Client::construct(move(pair.local), state, owner + 1);
        auto result = Fuzzing::dispatch(*clients[owner], CreateWebglContext { WebGLVersion::WebGL2, { 8, 8 }, false, false, false });
        if (!result.success())
            Fuzzing::unavailable("WebGL2/ANGLE context unavailable; configure the display/backend before fuzzing");
        canvases[owner] = result.canvas_id();
        buffers[owner] = MUST(WebGLSharedCommandBuffer::create(4096));
        Fuzzing::dispatch(*clients[owner], WebglSetCommandBuffer { canvases[owner], buffers[owner].buffer() });
        fill(owner, owner + 1);
    };
    create(0);
    create(1);
    Fuzzing::BoundedInput input({ data, size });
    for (size_t step = 0; step < 32 && !input.remaining().is_empty(); ++step) {
        size_t owner = input.byte() % 2;
        auto operation = input.byte() % 6;
        switch (operation) {
        case 0:
            fill(owner, input.byte());
            break;
        case 1: {
            u32 object = 1;
            Array<u8, 16> contents;
            for (auto& byte : contents)
                byte = input.byte();
            WebGLCommandList commands;
            commands.append(Commands::GenBuffers { 1, { WebGLCommandList::first_inline_data_offset(sizeof(Commands::GenBuffers)), sizeof(object) } }, { &object, sizeof(object) });
            commands.append(Commands::BindBuffer { 0x8892, object }); // GL_ARRAY_BUFFER
            commands.append(Commands::BufferData { 0x8892, contents.size(), true, { WebGLCommandList::first_inline_data_offset(sizeof(Commands::BufferData)), contents.size() }, 0x88e4 }, contents.span());
            publish(owner, commands);
            auto readback = MUST(Core::AnonymousBuffer::create_with_size(contents.size()));
            auto result = Fuzzing::dispatch(*clients[owner], WebglReadBufferSubData { canvases[owner], 0x8892, 0, contents.size(), readback });
            VERIFY(result.success());
            VERIFY(__builtin_memcmp(readback.data<void>(), contents.data(), contents.size()) == 0);
            WebGLCommandList deletion;
            deletion.append(Commands::DeleteBuffers { 1, { WebGLCommandList::first_inline_data_offset(sizeof(Commands::DeleteBuffers)), sizeof(object) } }, { &object, sizeof(object) });
            publish(owner, deletion);
            break;
        }
        case 2: {
            // Keep the sender's old alias alive across replacement; acknowledgements
            // must go to the current mapping, not the previously installed one.
            auto old = move(buffers[owner]);
            auto old_sequence = old.executed_flush_sequence_number();
            buffers[owner] = MUST(WebGLSharedCommandBuffer::create(4096));
            Fuzzing::dispatch(*clients[owner], WebglSetCommandBuffer { canvases[owner], buffers[owner].buffer() });
            __builtin_memset(old.data_region().data(), 0xff, old.data_region().size());
            fill(owner, input.byte());
            VERIFY(old.executed_flush_sequence_number() == old_sequence);
            break;
        }
        case 3: {
            auto old_id = canvases[owner];
            create(owner);
            VERIFY(canvases[owner] != old_id);
            VERIFY(!state->canvas_surface_registry().canvas_surface(old_id));
            break;
        }
        case 4: {
            // Deterministic malformed range, without random driver allocations.
            auto bad_offset = (input.byte() & 1) ? u64 { 1 } : NumericLimits<u64>::max();
            auto old_sequence = buffers[owner].executed_flush_sequence_number();
            Fuzzing::dispatch(*clients[owner], WebglCommandsFromSharedBuffer { canvases[owner], bad_offset, 16, ++sequence, {} });
            VERIFY(!clients[owner]->is_open());
            VERIFY(buffers[owner].executed_flush_sequence_number() == old_sequence);
            create(owner);
            break;
        }
        case 5: {
            // Mutate the sender-retained bytes after registration, before dispatch.
            // This is a deterministic alias test, not a claim of concurrent TOCTOU coverage.
            WebGLCommandHeader header { static_cast<WebGLCommandType>(webgl_command_type_count), 0 };
            __builtin_memcpy(buffers[owner].data_region().data(), &header, sizeof(header));
            auto old_sequence = buffers[owner].executed_flush_sequence_number();
            Fuzzing::dispatch(*clients[owner], WebglCommandsFromSharedBuffer { canvases[owner], 0, sizeof(header), ++sequence, {} });
            VERIFY(!clients[owner]->is_open());
            VERIFY(buffers[owner].executed_flush_sequence_number() == old_sequence);
            create(owner);
            break;
        }
        }
        for (size_t victim = 0; victim < 2; ++victim) {
            auto reply = Fuzzing::dispatch(*clients[victim], GetCanvasPixels { canvases[victim], { 0, 0, 8, 8 } });
            VERIFY(reply.pixels().is_valid());
            auto const& bitmap = *reply.pixels().bitmap();
            VERIFY(bitmap.size() == Gfx::IntSize(8, 8));
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    VERIFY(bitmap.get_pixel(x, y) == colors[victim]);
        }
    }
    for (auto& client : clients) {
        client->shutdown();
        client = nullptr;
    }
    for (auto const& id : canvases)
        VERIFY(!state->canvas_surface_registry().canvas_surface(id));
    return 0;
}
