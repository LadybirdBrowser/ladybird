/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>

namespace Compositing {

namespace RustFFI {

struct FfiDisplayListReplayCallbacks;

}

class COMPOSITING_API DisplayList : public AtomicRefCounted<DisplayList> {
public:
    ~DisplayList();
    struct AsyncScrollingMetadata {
        Gfx::IntRect viewport_rect;
        u64 wheel_event_listener_state_generation { 0 };
        bool has_blocking_wheel_event_listeners { false };
        bool has_blocking_wheel_event_region_covering_viewport { false };
        // Converts the compositor's device pixel offsets to the CSS pixels the snap geometry is in.
        double device_pixels_per_css_pixel { 1.0 };
        Compositing::KeyboardScrollState keyboard_scroll_state {};
    };

    // Everything about a list that travels alongside its tape when it is sent to the Compositor.
    struct Properties {
        u64 id { 0 };
        u64 compatible_visual_context_tree_structural_epoch { 0 };
        Optional<Gfx::Color> surface_clear_color;
        Optional<AsyncScrollingMetadata> async_scrolling_metadata;
    };

    // An empty list.
    static NonnullRefPtr<DisplayList> create(AccumulatedVisualContextTree const&);
    // Adopts a strong reference to an immutable Rust recording, including its run table.
    static NonnullRefPtr<DisplayList> adopt_rust_command_storage(AccumulatedVisualContextTree const&, void const*);
    // Shares an immutable Rust recording that someone else holds, taking a strong reference of its own.
    static NonnullRefPtr<DisplayList> share_rust_command_storage(AccumulatedVisualContextTree const&, void const*);

    // The producer's side of sending a list: a fresh shared buffer holding the tape and the run table. The
    // buffer is handed to the receiver whole; the producer keeps nothing.
    ErrorOr<Core::AnonymousBuffer> copy_to_shared_buffer() const;
    // The receiver's side: copies the tape and the run table out of the buffer, which the sender can still
    // write to, and checks the copies. Fails when the sizes do not fit the buffer or the tape is malformed.
    static ErrorOr<NonnullRefPtr<DisplayList>> create_from_shared_buffer(Properties, Core::AnonymousBuffer, u64 tape_size, u64 run_count);

    // Taken at send time: the async scrolling metadata is restamped on every send.
    Properties properties() const;

    u64 compatible_visual_context_tree_structural_epoch() const { return m_compatible_visual_context_tree_structural_epoch; }
    u64 id() const { return m_id; }

    // The Rust storage that holds the list's tape.
    void const* rust_handle() const { return m_storage; }

    // The sizes a receiver needs to unpack the list from its shared buffer.
    u64 tape_size() const;
    u64 run_count() const;

    void set_surface_clear_color(Gfx::Color color) { m_surface_clear_color = color; }
    Optional<Gfx::Color> surface_clear_color() const { return m_surface_clear_color; }
    void set_async_scrolling_metadata(AsyncScrollingMetadata metadata) { m_async_scrolling_metadata = metadata; }
    Optional<AsyncScrollingMetadata> const& async_scrolling_metadata() const { return m_async_scrolling_metadata; }

    // The ids of the resources the list's commands reference, including the commands nested in others, each once.
    struct ReferencedResourceIds {
        ReadonlySpan<u64> fonts;
        ReadonlySpan<u64> image_frames;
        ReadonlySpan<u64> video_sinks;
        ReadonlySpan<u64> display_lists;
    };
    ReferencedResourceIds referenced_resource_ids() const;
    // Whether reusing a raster of the list can produce different pixels than replaying it in place, leaving out the
    // display lists it nests.
    bool requires_direct_replay_without_nested_lists(AccumulatedVisualContextTree const&) const;
    // Visits the compositor metadata commands with the context of their run and their payload, in tape order.
    void for_each_compositor_metadata(Function<void(ContextRef, DisplayListCommandType, ReadonlyBytes payload)> const&) const;
    // Visits the canvases and carets the list draws outside of any group, with the context of their run and their
    // bounding rect.
    void for_each_drawn_canvas(Function<void(ContextRef, Optional<Gfx::IntRect>, DrawCanvas const&)> const&) const;
    void for_each_caret(Function<void(ContextRef, Optional<Gfx::IntRect>, PaintCaret const&)> const&) const;

    // Replays the list through a player's callbacks, against the visual context tree it was made for.
    void replay(AccumulatedVisualContextTree const&, ScrollStateSnapshot const&, RustFFI::FfiDisplayListReplayCallbacks const&) const;
    // Replays a stream of records that a command of a list being replayed nests.
    static void replay_records(ReadonlyBytes records, ScrollStateSnapshot const&, RustFFI::FfiDisplayListReplayCallbacks const&);

private:
    // Takes over the storage of a tape that came from another process and passed its checks.
    static NonnullRefPtr<DisplayList> adopt_received_storage(Properties, void const* storage);

    DisplayList(u64 compatible_visual_context_tree_structural_epoch, u64 id, void const* storage, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata>);

    u64 m_compatible_visual_context_tree_structural_epoch { 0 };
    u64 m_id { 0 };
    // One strong reference to the Rust storage, which owns the tape.
    void const* m_storage { nullptr };
    Optional<Gfx::Color> m_surface_clear_color;
    Optional<AsyncScrollingMetadata> m_async_scrolling_metadata;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

COMPOSITING_API ErrorOr<void> validate_display_list_references_live_visual_context_nodes(DisplayList const&, AccumulatedVisualContextTree const&);

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayList::AsyncScrollingMetadata const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayList::AsyncScrollingMetadata> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayList::Properties const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayList::Properties> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayList const&);
template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, NonnullRefPtr<Compositing::DisplayList> const&);
template<>
COMPOSITING_API ErrorOr<NonnullRefPtr<Compositing::DisplayList>> decode(Decoder&);

}
