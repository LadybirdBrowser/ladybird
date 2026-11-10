/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Point.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

// An index far above any real node count, but still inside the range the decoder accepts. Densifying it would
// allocate (index + 1) * sizeof(Gfx::FloatPoint), i.e. 512 MiB — the allocation the node-count bound exists to stop.
static constexpr u32 malicious_index = 0x04000000; // 2^26

struct WirePair {
    u64 index { 0 };
    Gfx::FloatPoint offset;
};

static ScrollStateSnapshot decode_snapshot(Vector<WirePair> const& pairs)
{
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(static_cast<u64>(pairs.size()))); // pair_count
    for (auto const& pair : pairs) {
        MUST(encoder.encode(pair.index));
        MUST(encoder.encode(pair.offset));
    }

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(IPC::decode<ScrollStateSnapshot>(decoder));
}

TEST_CASE(binding_to_node_count_keeps_in_range_and_drops_out_of_range)
{
    // #10847: a single 16-byte wire pair can claim any node index the decoder accepts. The decoder stages the pair
    // instead of densifying — so decoding cannot abort or over-allocate. The index is validated only once the
    // compositor supplies the node count, which drops it.
    auto snapshot = decode_snapshot({
        { 3, Gfx::FloatPoint { 10, 20 } },
        { malicious_index, Gfx::FloatPoint { 30, 40 } },
    });

    snapshot.set_node_count(8);

    // The in-range pair survives, the out-of-range pair is gone, and the dense store never spans more than the node count.
    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { 3 }), (Gfx::FloatPoint { 10, 20 }));
    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { malicious_index }), Gfx::FloatPoint {});
    EXPECT(snapshot.device_offsets().size() <= 8u);
}

TEST_CASE(set_device_offset_ignores_index_at_or_past_node_count)
{
    ScrollStateSnapshot snapshot;
    snapshot.set_node_count(4);

    // A valid index is stored; an index at the bound and one far past it are both ignored, so the dense store cannot
    // extend past the node count.
    snapshot.set_device_offset_for_index(SpatialNodeIndex { 3 }, Gfx::FloatPoint { 5, 6 });
    snapshot.set_device_offset_for_index(SpatialNodeIndex { 4 }, Gfx::FloatPoint { 7, 8 });
    snapshot.set_device_offset_for_index(SpatialNodeIndex { malicious_index }, Gfx::FloatPoint { 9, 9 });

    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { 3 }), (Gfx::FloatPoint { 5, 6 }));
    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { 4 }), Gfx::FloatPoint {});
    EXPECT(snapshot.device_offsets().size() <= 4u);
}

TEST_CASE(ipc_round_trip_preserves_scroll_offsets)
{
    ScrollStateSnapshot snapshot;
    snapshot.set_device_offset_for_index(SpatialNodeIndex { 3 }, Gfx::FloatPoint { 10, 20 });

    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(snapshot));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };

    auto decoded = MUST(IPC::decode<ScrollStateSnapshot>(decoder));
    decoded.set_node_count(8);
    EXPECT_EQ(decoded.device_offset_for_index(SpatialNodeIndex { 3 }), (Gfx::FloatPoint { 10, 20 }));
    // Indices that were never set are holes that decode back to zero offsets.
    EXPECT_EQ(decoded.device_offset_for_index(SpatialNodeIndex { 1 }), Gfx::FloatPoint {});
}
