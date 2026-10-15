/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <Compositor/KeyboardScrollController.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <math.h>

namespace Compositor {

void KeyboardScrollController::invalidate_state(u64 generation)
{
    if (generation < m_state.generation)
        return;
    m_state.generation = generation;
    m_state.target.clear();
}

bool KeyboardScrollController::adopt_state(Compositing::KeyboardScrollState state)
{
    if (state.generation < m_state.generation)
        return false;
    m_state = move(state);
    return true;
}

Optional<KeyboardScrollController::KeyStep> KeyboardScrollController::step_for_key(Web::KeyEvent const& event, Compositing::AsyncScrollTree const& async_scroll_tree) const
{
    VERIFY(m_state.target.has_value());
    auto modifiers_without_keypad = static_cast<u32>(event.modifiers) & ~Web::UIEvents::Mod_Keypad;
    bool scrolls_to_start = event.key == Web::UIEvents::Key_Home || (event.key == Web::UIEvents::Key_Up && modifiers_without_keypad == Web::UIEvents::Mod_PlatformCtrl);
    bool scrolls_to_end = event.key == Web::UIEvents::Key_End || (event.key == Web::UIEvents::Key_Down && modifiers_without_keypad == Web::UIEvents::Mod_PlatformCtrl);
    bool scrolls_to_extent = scrolls_to_start || scrolls_to_end;
    bool is_arrow = !scrolls_to_extent && first_is_one_of(event.key, Web::UIEvents::Key_Up, Web::UIEvents::Key_Down, Web::UIEvents::Key_Left, Web::UIEvents::Key_Right);
    Gfx::FloatPoint delta;
    if (scrolls_to_extent) {
        delta = async_scroll_tree.device_offset_from_css_pixels({ 0, scrolls_to_start ? -Web::CSSPixels::max() : Web::CSSPixels::max() });
    } else {
        auto distance = is_arrow ? m_state.arrow_scroll_distance : m_state.page_scroll_distance;
        if (!isfinite(distance) || distance <= 0)
            return {};
        if (event.key == Web::UIEvents::KeyCode::Key_PageUp
            || event.key == Web::UIEvents::KeyCode::Key_Up || event.key == Web::UIEvents::KeyCode::Key_Left
            || (event.key == Web::UIEvents::KeyCode::Key_Space && (event.modifiers & Web::UIEvents::Mod_Shift)))
            distance = -distance;
        bool is_horizontal = first_is_one_of(event.key, Web::UIEvents::Key_Left, Web::UIEvents::Key_Right);
        delta = is_horizontal ? Gfx::FloatPoint { distance, 0 } : Gfx::FloatPoint { 0, distance };
    }

    auto snap_selection_intent = Compositing::SnapSelectionStrategy::Type::EndPositionAndDirection;
    if (scrolls_to_extent)
        snap_selection_intent = Compositing::SnapSelectionStrategy::Type::EndPosition;
    else if (is_arrow)
        snap_selection_intent = Compositing::SnapSelectionStrategy::Type::Direction;

    return KeyStep {
        .keyboard_scroll_target = *m_state.target,
        .delta = delta,
        .scrolls_to_extent = scrolls_to_extent,
        .snap_selection_intent = snap_selection_intent,
    };
}

void KeyboardScrollController::hold_key(Web::UIEvents::KeyCode key)
{
    if (!m_held_keys.contains_slow(key))
        m_held_keys.append(key);
}

KeyboardScrollController::KeyRelease KeyboardScrollController::release_key(Web::KeyEvent const& event)
{
    if (event.repeat || !m_held_keys.remove_first_matching([&](auto key) { return key == event.key; }))
        return KeyRelease::Ignored;
    if (m_held_keys.is_empty())
        return KeyRelease::ReleasedLastHeldKey;
    return KeyRelease::ReleasedWhileOtherKeysAreHeld;
}

bool KeyboardScrollController::release_all_keys()
{
    if (m_held_keys.is_empty())
        return false;
    m_held_keys.clear();
    return true;
}

}
