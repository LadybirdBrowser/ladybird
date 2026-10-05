/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibWeb/Compositor/CompositorFrame.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Painting/DisplayListRecording.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Web::Layout::RustFFI {

struct FfiPresentedRecording;

}

namespace Web::Compositor {

// A display list a recording published, and whether the recording made it the next one's paint command cache source.
struct PublishedDisplayList {
    NonnullRefPtr<Compositing::DisplayList> display_list;
    bool replaces_paint_command_cache_source { false };
};

// What presented a navigable's last frame: the main thread, or what presents beside it: a recording that flew, or a tick
// of a clock lease.
enum class PresentedBy : u8 {
    Main,
    Flight,
    Clock,
};

// What a frame is built from that its navigable and document held where the frame began, sealed there: the frame reaches
// neither of them again before it is presented.
struct SealedPresentation {
    AK_ALLOC_WITH_KMALLOC;

    // The recording's paint config.
    HTML::PaintConfig paint_config;
    // The visual context tree a new display list is cut from, or the one the compositor's display list takes where the
    // document's tree changed since it took one. Only a frame that sends a tree seals one.
    Optional<Compositing::AccumulatedVisualContextTree> visual_context_tree;
    // Whether the document's tree changed since the compositor took one.
    bool sends_visual_context_tree { false };
    // The scroll state, with the async scroll sequence the navigable adopted.
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    // What keyboard scrolling targets, for a top-level traversable.
    Optional<Compositing::KeyboardScrollState> keyboard_scroll_state;
    // The display list a recording copies paint commands from, and the resources it references, which stay live while
    // it does.
    RefPtr<Compositing::DisplayList> paint_command_cache_source;
    Compositing::DisplayListResourceSet paint_command_cache_source_resources;

    // For frames presented beside the event loop: the recording they publish, sealed where the recording began, where
    // they go and the rect they are presented in, what presents them, and, once one is presented, the display list it
    // published. A clock lease presents a frame from the seal at each tick that records one.
    Optional<Painting::DisplayListRecording> recording;
    RefPtr<CompositorFrameSink> sink;
    Web::CompositorContextId context_id;
    Optional<Gfx::IntRect> present_viewport_rect;
    PresentedBy presented_by { PresentedBy::Flight };
    Optional<PublishedDisplayList> published;
    // Whether a tick of a clock lease changed the document's visual context tree, which the host records again where no
    // tick published a frame.
    bool visual_context_tree_changed { false };
};

// What a navigable presents to its compositor context from: the resource storage its recordings add to, and the
// display list the compositor context holds with the resources it holds for it. The presenter holds no GC pointer.
class WEB_API NavigablePresenter {
    AK_MAKE_NONCOPYABLE(NavigablePresenter);
    AK_MAKE_NONMOVABLE(NavigablePresenter);

public:
    AK_ALLOC_WITH_KMALLOC;

    NavigablePresenter() = default;

    Compositing::DisplayListResourceStorage& display_list_resource_storage() { return m_resource_storage; }
    Compositing::DisplayListResourceStorage const& display_list_resource_storage() const { return m_resource_storage; }

    // The display list the compositor context holds, and the paint config it was recorded with.
    RefPtr<Compositing::DisplayList> const& compositor_display_list() const { return m_compositor_display_list; }
    Optional<HTML::PaintConfig> const& compositor_display_list_paint_config() const { return m_compositor_display_list_paint_config; }

    // Forgets what the compositor context holds: a new compositor process holds nothing.
    void forget_compositor_display_list();

    PresentedBy last_frame_presented_by() const { return m_last_frame_presented_by; }
    // The generation of the keyboard scroll state the last frame handed the compositor, if it handed one.
    Optional<u64> last_keyboard_scroll_state_generation() const { return m_last_keyboard_scroll_state_generation; }

    // Builds the frame that brings the compositor context up to date with `published`, the display list a recording just
    // published, or with what changed for the one the compositor has where none was published, from what `sealed`
    // sealed. Reads no navigable or document.
    CompositorFrame build_frame(SealedPresentation const&, Optional<PublishedDisplayList>);

    // Presents the frame `sealed` sealed, with `display_list`, the display list its recording published, beside the event
    // loop: through the seal's sink, keeping what it published in the seal, which the next frame from the seal copies
    // paint commands from.
    void present_beside_event_loop(SealedPresentation&, NonnullRefPtr<Compositing::DisplayList>);

private:
    Compositing::DisplayListResourceStorage m_resource_storage;
    Optional<HTML::PaintConfig> m_compositor_display_list_paint_config;
    RefPtr<Compositing::DisplayList> m_compositor_display_list;
    u64 m_compositor_display_list_visual_context_tree_structural_epoch { 0 };
    Compositing::DisplayListResourceSet m_compositor_display_list_resources;
    Compositing::DisplayListResourceSet m_compositor_display_list_command_resources;
    PresentedBy m_last_frame_presented_by { PresentedBy::Main };
    Optional<u64> m_last_keyboard_scroll_state_generation;
};

// A navigable's presenter and a frame sealed for it, which what presents the frame beside the event loop owns until it
// lands.
struct FlightPresentation {
    NonnullOwnPtr<NavigablePresenter> presenter;
    NonnullOwnPtr<SealedPresentation> sealed;
};

}

// What a recording that presents beside the event loop reaches of its presenter and seal, which it owns meanwhile.
extern "C" {
WEB_API void web_navigable_presenter_destroy(void* presenter);
WEB_API void web_sealed_presentation_destroy(void* sealed);
WEB_API void web_sealed_presentation_take_visual_context_tree(void* sealed, void const* tree, Gfx::FloatPoint const* restructured_scroll_offsets, size_t scroll_offset_count);
WEB_API void web_sealed_presentation_note_visual_context_tree_changed(void* sealed);
WEB_API void web_navigable_presenter_add_font(void* presenter, void const* font);
WEB_API void web_navigable_presenter_add_image_frame(void* presenter, void const* frame);
WEB_API void web_navigable_presenter_add_video_sink(void* presenter, u64 resource_id, u64 sink_handle);
WEB_API void web_navigable_presenter_present(void* presenter, void* sealed, Web::Layout::RustFFI::FfiPresentedRecording const* presented);
}
