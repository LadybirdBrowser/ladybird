/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListProperties.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>

namespace Compositing {

namespace RustFFI {

struct FfiDisplayList;
struct FfiRasterPainter;

}

class COMPOSITING_API DisplayList : public AtomicRefCounted<DisplayList> {
public:
    ~DisplayList();

    // An empty list, recorded against a visual context tree with no node but the visual viewport.
    static NonnullRefPtr<DisplayList> create_empty();
    // Adopts a strong reference to the Rust storage of a list recorded in this process, which holds the visual context
    // tree the list was recorded against.
    static NonnullRefPtr<DisplayList> adopt_rust_storage(void const* retained_storage);
    // Adopts a strong reference to an immutable Rust recording, including its run table, and one to the visual context
    // tree it was recorded against. Rust hands both over, and only Rust reads the tree.
    static NonnullRefPtr<DisplayList> adopt_rust_recording(void const* retained_commands, void const* retained_visual_context_tree);
    // Shares an immutable Rust recording, and the visual context tree it was recorded against, that Rust holds and
    // lends for the call, taking a strong reference of its own to each.
    static NonnullRefPtr<DisplayList> share_rust_recording(void const* lent_commands, void const* lent_visual_context_tree);

    // The producer's side of sending a list: a fresh shared buffer holding the tape and the run table. The
    // buffer is handed to the receiver whole; the producer keeps nothing.
    ErrorOr<Core::AnonymousBuffer> copy_to_shared_buffer() const;
    // The receiver's side: copies the tape and the run table out of the buffer, which the sender can still
    // write to, and checks the copies. Fails when the sizes do not fit the buffer or the tape is malformed.
    static ErrorOr<NonnullRefPtr<DisplayList>> create_from_shared_buffer(DisplayListProperties, Core::AnonymousBuffer, u64 tape_size, u64 run_count);

    DisplayListProperties properties() const;

    u64 compatible_visual_context_tree_structural_epoch() const { return m_compatible_visual_context_tree_structural_epoch; }
    u64 id() const { return m_id; }

    // The Rust storage that holds the list's tape.
    void const* rust_handle() const { return m_storage; }
    // The list as the scroll logic takes it, with the async scrolling metadata it goes with as IPC encodes an
    // Optional of it.
    RustFFI::FfiDisplayList for_scroll_logic(ReadonlyBytes encoded_async_scrolling_metadata) const;

    // The sizes a receiver needs to unpack the list from its shared buffer.
    u64 tape_size() const;
    u64 run_count() const;

    void set_surface_clear_color(Gfx::Color color) { m_surface_clear_color = color; }
    Optional<Gfx::Color> surface_clear_color() const { return m_surface_clear_color; }
    void set_root_group_color(Gfx::Color color) { m_root_group_color = color; }
    Gfx::Color root_group_color() const { return m_root_group_color; }
    void set_async_scrolling_metadata(AsyncScrollingMetadata metadata) { m_async_scrolling_metadata = metadata; }
    Optional<AsyncScrollingMetadata> const& async_scrolling_metadata() const { return m_async_scrolling_metadata; }

    // The ids of the resources the list's commands reference, including the commands nested in others, each once.
    struct ReferencedResourceIds {
        ReadonlySpan<u64> fonts;
        ReadonlySpan<u64> image_frames;
        ReadonlySpan<u64> video_sinks;
        ReadonlySpan<u64> display_lists;
        ReadonlySpan<u64> canvases;
    };
    ReferencedResourceIds referenced_resource_ids() const;

    // Whether the list was recorded against the visual context tree Rust lends for the call, not a copy of it.
    bool was_recorded_against(void const* lent_visual_context_tree) const;
    // The visual context tree a list recorded in this process was recorded against, serialized for the compositor.
    ByteBuffer serialized_visual_context_tree() const;
    // The ids of the image frames that the filters and backdrop filters of the effects of that tree draw.
    void for_each_visual_context_tree_filter_image_frame_id(Function<void(u64)> const&) const;

    // Replays a list recorded in this process through a painter, against the visual context tree it was recorded
    // against, with nothing scrolled and a blinking caret as it shows at the time given.
    void replay(RustFFI::FfiRasterPainter const&, i64 time_nanoseconds) const;

private:
    // Takes over the storage of a tape that came from another process and passed its checks.
    static NonnullRefPtr<DisplayList> adopt_received_storage(DisplayListProperties, void const* storage);

    DisplayList(u64 compatible_visual_context_tree_structural_epoch, u64 id, void const* storage, Optional<Gfx::Color> surface_clear_color, Gfx::Color root_group_color, Optional<AsyncScrollingMetadata>);

    u64 m_compatible_visual_context_tree_structural_epoch { 0 };
    u64 m_id { 0 };
    // One strong reference to the Rust storage, which owns the tape.
    void const* m_storage { nullptr };
    Optional<Gfx::Color> m_surface_clear_color;
    Gfx::Color m_root_group_color { Gfx::Color::Transparent };
    Optional<AsyncScrollingMetadata> m_async_scrolling_metadata;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayList const&);
template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, NonnullRefPtr<Compositing::DisplayList> const&);
template<>
COMPOSITING_API ErrorOr<NonnullRefPtr<Compositing::DisplayList>> decode(Decoder&);

}
