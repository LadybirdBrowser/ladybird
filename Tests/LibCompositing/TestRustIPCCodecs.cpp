/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

// A value encoded after the one under test, which must decode where the Rust codec left the decoder.
static constexpr u32 sentinel = 0xC0FFEE42;

static AccumulatedVisualContextTree make_tree()
{
    VisualContextTreeTestBuilder builder;
    auto scroll_node = builder.append_scroll(VISUAL_VIEWPORT_NODE_INDEX);
    (void)builder.append_transform(scroll_node, Gfx::scale_matrix(Gfx::FloatVector3 { 2, 2, 1 }), { 10, 20 });
    return builder.finish();
}

template<typename T>
static IPC::MessageDataType encode_followed_by_sentinel(T const& value)
{
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(value));
    MUST(encoder.encode(sentinel));
    EXPECT(message_buffer.take_attachments().is_empty());
    return message_buffer.take_data();
}

template<typename T>
static ErrorOr<T> decode_followed_by_sentinel(ReadonlyBytes bytes)
{
    FixedMemoryStream stream { bytes };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    auto value = TRY(decoder.decode<T>());
    EXPECT_EQ(TRY(decoder.decode<u32>()), sentinel);
    EXPECT_EQ(stream.remaining(), 0u);
    return value;
}

TEST_CASE(a_visual_context_tree_round_trips_and_leaves_the_decoder_after_it)
{
    auto tree = make_tree();
    auto data = encode_followed_by_sentinel(tree);

    auto decoded = MUST(decode_followed_by_sentinel<AccumulatedVisualContextTree>(data.span()));
    EXPECT_EQ(decoded.structural_epoch(), tree.structural_epoch());
    EXPECT_EQ(decoded.spatial_node_count(), tree.spatial_node_count());
    EXPECT_EQ(decoded.node_count(), tree.node_count());
    EXPECT_EQ(decoded.live_node_count(), tree.live_node_count());
}

TEST_CASE(a_visual_context_tree_goes_as_a_byte_buffer)
{
    auto data = encode_followed_by_sentinel(make_tree());

    // The size of the tree's bytes, the bytes, then the sentinel.
    VERIFY(data.size() >= 8);
    u32 size = 0;
    memcpy(&size, data.data(), sizeof(size));
    EXPECT_EQ(data.size(), sizeof(u32) + size + sizeof(sentinel));
}

TEST_CASE(a_truncated_visual_context_tree_does_not_decode)
{
    auto data = encode_followed_by_sentinel(make_tree());

    // Drop the sentinel and the last byte of the tree.
    auto truncated = data.span().trim(data.size() - sizeof(sentinel) - 1);
    FixedMemoryStream stream { truncated };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<AccumulatedVisualContextTree>().is_error());
}
