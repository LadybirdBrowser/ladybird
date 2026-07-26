/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibGfx/Point.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/VisualContextTreeTestBuilder.h>

using namespace Web::Painting;

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

TEST_CASE(decode_of_out_of_range_index_does_not_grow_the_dense_store)
{
    // #10847: a single 16-byte wire pair can claim any node index the decoder accepts. The decoder stages the pair
    // instead of densifying — so decoding cannot abort or over-allocate. The index is validated only once the
    // compositor supplies the node count, which drops it.
    auto snapshot = decode_snapshot({ { malicious_index, Gfx::FloatPoint { 1, 2 } } });

    snapshot.set_node_count(8);

    EXPECT(snapshot.device_offsets().size() <= 8u);
    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { malicious_index }), Gfx::FloatPoint {});
}

TEST_CASE(binding_to_node_count_keeps_in_range_and_drops_out_of_range)
{
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

TEST_CASE(node_count_comes_from_the_visual_context_tree)
{
    VisualContextTreeTestBuilder builder;
    builder.append_transform(VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    builder.append_transform(VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto tree = builder.finish();

    // The builder seeds the viewport node, so two appends leave three nodes; indices 0..2 are valid.
    EXPECT_EQ(tree.spatial_node_count(), 3u);

    auto snapshot = decode_snapshot({
        { 2, Gfx::FloatPoint { 1, 1 } },
        { 3, Gfx::FloatPoint { 2, 2 } },
    });
    snapshot.set_node_count(tree.spatial_node_count());

    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { 2 }), (Gfx::FloatPoint { 1, 1 }));
    EXPECT_EQ(snapshot.device_offset_for_index(SpatialNodeIndex { 3 }), Gfx::FloatPoint {});
    EXPECT(snapshot.device_offsets().size() <= tree.spatial_node_count());
}

static AccumulatedVisualContextTree make_tree_with_four_spatial_nodes()
{
    VisualContextTreeTestBuilder builder;
    // The builder seeds the viewport node, so three appends leave four.
    builder.append_transform(VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    builder.append_transform(VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    builder.append_transform(VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    return builder.finish();
}

static Web::Compositor::AsyncScrollNode make_scroll_node(SpatialNodeIndex scroll_node_index)
{
    Web::Compositor::AsyncScrollNodeID node_id {
        .document_id = Web::UniqueNodeID { 1 },
        .scroll_node_index = scroll_node_index,
    };
    return {
        .node_id = node_id,
        .stable_node_id = {
            .node_id = Web::UniqueNodeID { 1 },
            .kind = Web::Compositor::AsyncScrollNodeKind::Element,
            .pseudo_element_type = 0,
        },
        .parent_node_id = {},
        .scrollport_rect = Gfx::IntRect { 0, 0, 100, 100 },
        .min_scroll_offset = Gfx::FloatPoint { 0, 0 },
        .max_scroll_offset = Gfx::FloatPoint { 100, 100 },
        .is_viewport = false,
        .can_be_wheel_scrolled_horizontally = true,
        .can_be_wheel_scrolled_vertically = true,
    };
}

TEST_CASE(async_scroll_tree_ignores_out_of_range_scroll_node_index)
{
    // The second ingress: a crafted CompositorScrollNode command in the display list gives the scroll tree an out-of-
    // range scroll node index — which apply_scroll_delta feeds to the same dense-store setter. The node-count bound on
    // the snapshot drops it, instead of aborting.
    Web::Compositor::AsyncScrollingState state;
    auto out_of_range_index = SpatialNodeIndex { malicious_index };
    state.scroll_nodes.append(make_scroll_node(out_of_range_index));

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));

    auto tree = make_tree_with_four_spatial_nodes();
    ScrollStateSnapshot snapshot;
    snapshot.set_node_count(tree.spatial_node_count());
    (void)scroll_tree.apply_scroll_delta(
        Web::Compositor::AsyncScrollNodeID { .document_id = Web::UniqueNodeID { 1 }, .scroll_node_index = out_of_range_index },
        Gfx::FloatPoint { 10, 10 },
        tree,
        snapshot);

    EXPECT(snapshot.device_offsets().size() <= 4u);
    EXPECT_EQ(snapshot.device_offset_for_index(out_of_range_index), Gfx::FloatPoint {});
}

TEST_CASE(async_scroll_tree_records_in_range_scroll_node_index)
{
    // The same path with a valid index still writes the offset, confirming the bound only rejects out-of-range indices,
    // rather than disabling async scrolling.
    Web::Compositor::AsyncScrollingState state;
    auto in_range_index = SpatialNodeIndex { 2 };
    state.scroll_nodes.append(make_scroll_node(in_range_index));

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));

    auto tree = make_tree_with_four_spatial_nodes();
    ScrollStateSnapshot snapshot;
    snapshot.set_node_count(tree.spatial_node_count());
    (void)scroll_tree.apply_scroll_delta(
        Web::Compositor::AsyncScrollNodeID { .document_id = Web::UniqueNodeID { 1 }, .scroll_node_index = in_range_index },
        Gfx::FloatPoint { 10, 10 },
        tree,
        snapshot);

    EXPECT(snapshot.device_offsets().size() <= 4u);
    EXPECT_NE(snapshot.device_offset_for_index(in_range_index), Gfx::FloatPoint {});
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
