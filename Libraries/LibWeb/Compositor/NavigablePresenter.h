/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Web::Layout::RustFFI {

struct FfiSeal;
struct NavigablePresenter;

}

namespace Web::Compositor {

// What presented a navigable's last frame: the recording of a frame a rendering update committed, or a tick of the
// document's clock lane.
enum class PresentedBy : u8 {
    Commit,
    Clock,
};

// What a frame is built from that its navigable and document held where the frame began, sealed there: the frame reaches
// neither of them again before it is presented. What presents the frame takes it over as the frame is committed.
struct SealedPresentation {
    // The recording's paint config.
    HTML::PaintConfig paint_config;
    // The structural epoch of the visual context tree a new display list is cut from.
    Optional<u64> visual_context_tree_structural_epoch;
    // Whether the document's tree changed since the compositor took one.
    bool sends_visual_context_tree { false };
    // The scrolls the navigable asked for since the frame before, which the compositor applies once it has the frame's
    // boxes.
    Vector<Compositing::ScrollRequest> scroll_requests;
    // What keyboard scrolling targets, for a top-level traversable.
    Optional<Compositing::KeyboardScrollState> keyboard_scroll_state;
    Web::CompositorContextId context_id;
    Optional<Gfx::IntRect> present_viewport_rect;
    // Whether the frame sends a visual context tree the compositor's display list was not recorded against, for a test.
    bool visual_context_tree_mismatches_for_testing { false };
    // The frame a rendering update committed, counted per navigable.
    u64 generation { 0 };
};

// A navigable's presenter, which keeps what the navigable's compositor context holds. The Paint thread presents through
// it, and the main thread reads what it presented here.
class WEB_API NavigablePresenter {
    AK_MAKE_NONCOPYABLE(NavigablePresenter);
    AK_MAKE_NONMOVABLE(NavigablePresenter);

public:
    AK_ALLOC_WITH_KMALLOC;

    NavigablePresenter();
    ~NavigablePresenter();

    // The paint config of the display list the compositor context holds, and the structural epoch of the visual context
    // tree it was recorded against.
    Optional<HTML::PaintConfig> compositor_display_list_paint_config() const;
    Optional<u64> compositor_display_list_structural_epoch() const;

    // Forgets what the compositor context holds: a new compositor process holds nothing.
    void forget_compositor_display_list();

    Optional<PresentedBy> last_frame_presented_by() const;
    String display_list_window_scroll_positions_for_testing() const;
    // The generation of the keyboard scroll state the last frame handed the compositor, if it handed one.
    Optional<u64> last_keyboard_scroll_state_generation() const;

    // Lends `take` the seal of a frame of this presenter as Rust takes it, for the call.
    void lend_seal(SealedPresentation const&, Function<void(Layout::RustFFI::FfiSeal const&)> const& take) const;

private:
    Layout::RustFFI::NavigablePresenter const* m_presenter { nullptr };
};

// A navigable's presenter and a frame sealed for it, which what presents the frame beside the event loop takes over as
// the frame is committed.
struct FlightPresentation {
    NavigablePresenter const* presenter;
    SealedPresentation sealed;
};

}
