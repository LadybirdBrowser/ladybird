/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/MemoryStream.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size > 16384)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    Gfx::CanvasCommandList commands;
    for (size_t i = 0; i < 64 && !input.remaining().is_empty(); ++i) {
        auto operation = input.byte() % 8;
        auto x = static_cast<float>(input.byte()) - 128;
        auto y = static_cast<float>(input.byte()) - 128;
        auto width = static_cast<float>(input.byte());
        auto height = static_cast<float>(input.byte());
        auto red = input.byte();
        auto green = input.byte();
        auto blue = input.byte();
        auto alpha = input.byte();
        Gfx::Color color(red, green, blue, alpha);
        switch (operation) {
        case 0:
            commands.append(Gfx::CanvasCommands::Save {});
            break;
        case 1:
            commands.append(Gfx::CanvasCommands::Restore {});
            break;
        case 2:
            commands.append(Gfx::CanvasCommands::Reset {});
            break;
        case 3:
            commands.append(Gfx::CanvasCommands::FillRect { { x, y, width, height }, color });
            break;
        case 4:
            commands.append(Gfx::CanvasCommands::ClearRect { { x, y, width, height }, color });
            break;
        case 5:
            commands.append(Gfx::CanvasCommands::SetTransform { Gfx::AffineTransform { x, y, width, height, -x, -y } });
            break;
        case 6: {
            Gfx::Path path;
            path.move_to({ x, y });
            path.line_to({ width, height });
            path.close();
            commands.append(Gfx::CanvasCommands::ClipPath { move(path), Gfx::WindingRule::EvenOdd });
            break;
        }
        case 7: {
            Gfx::CanvasCommands::DrawGlyphRun run {
                .font_id = red,
                .glyphs = {},
                .translation = { width, height },
                .style = color,
                .filter = {},
            };
            run.glyphs.append({ { x, y }, green });
            commands.append(move(run));
            break;
        }
        }
    }
    IPC::MessageBuffer wire;
    IPC::Encoder encoder(wire);
    MUST(encoder.encode(commands));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream(wire.data().span());
    IPC::Decoder decoder(stream, attachments);
    auto decoded = MUST(decoder.decode<Gfx::CanvasCommandList>());
    VERIFY(decoded.size() == commands.size());
    VERIFY(stream.is_eof());
    IPC::MessageBuffer second;
    IPC::Encoder second_encoder(second);
    MUST(second_encoder.encode(decoded));
    VERIFY(wire.data().span() == second.data().span());
    // Test truncation with real tags/counts, not huge synthetic allocation sizes.
    auto cutoff = size == 0 ? 0 : static_cast<size_t>(data[size - 1]) * wire.data().size() / 256;
    FixedMemoryStream truncated(wire.data().span().slice(0, cutoff));
    IPC::Decoder truncated_decoder(truncated, attachments);
    VERIFY(truncated_decoder.decode<Gfx::CanvasCommandList>().is_error());
    return 0;
}
