/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromWebContent.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/CompositorConnection.h>
#include <Tests/LibCompositing/DisplayListTestHelpers.h>

TEST_CASE(non_windows_transport_initialization_disconnects_web_content)
{
    Core::EventLoop event_loop;
    auto paired_transport = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto client_transport = TRY_OR_FAIL(paired_transport.remote_handle.create_transport());
    auto client = adopt_ref(*new Web::Compositor::CompositorConnection(move(client_transport)));
    auto compositor_state = Compositor::CompositorState::create({});
    auto connection = Compositor::ConnectionFromWebContent::construct(move(paired_transport.local), move(compositor_state), 1);

    bool disconnected = false;
    connection->set_on_death([&](auto&) {
        disconnected = true;
    });

    TRY_OR_FAIL(client->post_message(Messages::CompositorWebContentServer::InitTransport(42)));
    client->transport().flush();
    connection->dispatch_pending_messages();

    EXPECT(disconnected);
    EXPECT(!connection->is_open());
}

// A test of the transport has no Paint thread, and hands its frames to the compositor itself.
struct Web::Compositor::TransportTestAccess {
    static void submit_frame(CompositorConnection& connection, CompositorFrame&& frame)
    {
        connection.submit_frame_for_testing(move(frame));
    }
};

namespace {

// A WebContent connection talking to a Compositor over a real transport pair, with one context owned by it.
struct SharedDisplayListFixture {
    static constexpr Web::CompositorContextId context_id { 1 };

    SharedDisplayListFixture()
        : visual_context_tree(Compositing::VisualContextTreeTestBuilder().finish())
    {
        auto paired_transport = MUST(IPC::Transport::create_paired());
        client = adopt_ref(*new Web::Compositor::CompositorConnection(MUST(paired_transport.remote_handle.create_transport())));
        compositor_state = Compositor::CompositorState::create({});
        connection = Compositor::ConnectionFromWebContent::construct(move(paired_transport.local), NonnullRefPtr { *compositor_state }, 1);
        connection->set_on_death([this](auto&) { disconnected = true; });
        compositor_state->create_context(context_id, {}, *connection);
    }

    void pump()
    {
        client->transport().flush();
        connection->dispatch_pending_messages();
    }

    TestDisplayList fill_rects(size_t count)
    {
        TestDisplayList commands;
        for (size_t i = 0; i < count; ++i)
            append_display_list_command(commands, Compositing::FillRect { { 0, 0, 8, 8 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, Compositing::NO_EFFECT_NODE }, Gfx::IntRect { 0, 0, 8, 8 }, Compositing::ContextRef { Compositing::SpatialNodeIndex { 0 }, Compositing::NO_CLIP_NODE, Compositing::NO_EFFECT_NODE });
        return commands;
    }

    NonnullRefPtr<Compositing::DisplayList> list_of(size_t fill_rect_count)
    {
        auto commands = fill_rects(fill_rect_count);
        return Compositing::DisplayList::create_from_command_bytes(visual_context_tree, move(commands.bytes), move(commands.runs));
    }

    // Posts the update message by hand so a test can lie about the sizes.
    void post_update(Compositing::DisplayList const& display_list, u64 tape_size, u64 run_count, Optional<u64> epoch = {})
    {
        auto properties = display_list.properties();
        if (epoch.has_value())
            properties.compatible_visual_context_tree_structural_epoch = *epoch;
        auto buffer = MUST(display_list.copy_to_shared_buffer());
        MUST(client->post_message(Messages::CompositorWebContentServer::UpdateDisplayList(context_id, buffer, tape_size, run_count, properties, visual_context_tree, Compositing::DisplayListResourceTransaction {}, Compositing::ScrollStateSnapshot {})));
        pump();
    }

    Core::EventLoop event_loop;
    RefPtr<Web::Compositor::CompositorConnection> client;
    RefPtr<Compositor::CompositorState> compositor_state;
    RefPtr<Compositor::ConnectionFromWebContent> connection;
    Compositing::AccumulatedVisualContextTree visual_context_tree;
    bool disconnected { false };
};

}

TEST_CASE(a_display_list_larger_than_an_ipc_message_travels_through_shared_memory)
{
    SharedDisplayListFixture fixture;
    // Well past IPC::MAX_MESSAGE_PAYLOAD_SIZE, which the inline encoding could never carry.
    auto display_list = fixture.list_of((IPC::MAX_MESSAGE_PAYLOAD_SIZE + 8 * MiB) / 56);
    EXPECT(display_list->command_bytes().size() > IPC::MAX_MESSAGE_PAYLOAD_SIZE);

    Web::Compositor::CompositorFrame frame;
    frame.context_id = SharedDisplayListFixture::context_id;
    frame.display_list_update = Web::Compositor::CompositorFrame::DisplayListUpdate {
        .display_list = display_list,
        .visual_context_tree = fixture.visual_context_tree,
        .resource_transaction = {},
        .scroll_state_snapshot = {},
    };
    Web::Compositor::TransportTestAccess::submit_frame(*fixture.client, move(frame));
    fixture.pump();
    EXPECT(!fixture.disconnected);
    EXPECT(fixture.connection->is_open());
}

// Each case gets its own fixture, and therefore its own event loop, one at a time.
static bool update_with_sizes_disconnects(Function<void(Compositing::DisplayList const&, u64& tape_size, u64& run_count)> adjust_sizes)
{
    SharedDisplayListFixture fixture;
    auto list = fixture.list_of(3);
    u64 tape_size = list->command_bytes().size();
    u64 run_count = list->command_runs().size();
    adjust_sizes(*list, tape_size, run_count);
    fixture.post_update(*list, tape_size, run_count);
    return fixture.disconnected;
}

TEST_CASE(updates_whose_sizes_do_not_fit_their_buffer_disconnect_web_content)
{
    EXPECT(update_with_sizes_disconnects([](auto const&, u64& tape_size, u64&) { tape_size += 8; }));
    EXPECT(update_with_sizes_disconnects([](auto const&, u64& tape_size, u64&) { tape_size -= 4; }));
    EXPECT(update_with_sizes_disconnects([](auto const&, u64&, u64& run_count) { run_count += 1; }));
    EXPECT(!update_with_sizes_disconnects([](auto const&, u64&, u64&) { }));
}

TEST_CASE(an_update_for_a_stale_tree_is_dropped_without_disconnecting)
{
    SharedDisplayListFixture fixture;
    auto list = fixture.list_of(2);
    fixture.post_update(*list, list->command_bytes().size(), list->command_runs().size(), fixture.visual_context_tree.structural_epoch() + 1);
    EXPECT(!fixture.disconnected);
}
