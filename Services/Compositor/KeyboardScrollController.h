/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCompositing/Scrolling/ScrollSnapSelection.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositing {

class AsyncScrollTree;

}

namespace Compositor {

class KeyboardScrollController {
public:
    struct KeyStep {
        // Where the scroller chain the key scrolls starts.
        Web::AsyncScrollNodeStableID keyboard_scroll_target;
        Gfx::FloatPoint delta;
        bool scrolls_to_extent { false };
        Compositing::SnapSelectionStrategy::Type snap_selection_intent { Compositing::SnapSelectionStrategy::Type::EndPositionAndDirection };
    };

    enum class KeyRelease : u8 {
        Ignored,
        ReleasedWhileOtherKeysAreHeld,
        ReleasedLastHeldKey,
    };

    void invalidate_state(u64 generation);
    // Returns whether the state is not older than the one already adopted.
    bool adopt_state(Compositing::KeyboardScrollState);
    void forget_target() { m_state.target.clear(); }
    bool has_target() const { return m_state.target.has_value(); }
    bool state_matches_visual_context_tree_epoch(u64 visual_context_tree_structural_epoch) const { return m_state.visual_context_tree_structural_epoch == visual_context_tree_structural_epoch; }

    Optional<KeyStep> step_for_key(Web::KeyEvent const&, Compositing::AsyncScrollTree const&) const;

    void hold_key(Web::UIEvents::KeyCode);
    KeyRelease release_key(Web::KeyEvent const&);
    // Returns whether any key was held.
    bool release_all_keys();
    bool has_held_keys() const { return !m_held_keys.is_empty(); }

private:
    Compositing::KeyboardScrollState m_state;
    Vector<Web::UIEvents::KeyCode, 3> m_held_keys;
};

}
