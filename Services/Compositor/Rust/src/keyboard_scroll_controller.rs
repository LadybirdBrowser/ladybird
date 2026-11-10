/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcompositing_rust::css_pixels::{CssPixelPoint, CssPixels};
use libcompositing_rust::scrolling::scroll_snap_selection::SnapSelectionStrategyType;
use libcompositing_rust::scrolling::{
    AsyncScrollNodeStableID, AsyncScrollTree, KeyCode, KeyEvent, KeyModifier, KeyboardScrollState,
};
use libgfx_rust::FloatPoint;

pub struct KeyboardScrollController {
    state: KeyboardScrollState,
    held_keys: Vec<KeyCode>,
}

pub struct KeyStep {
    // Where the scroller chain the key scrolls starts.
    pub keyboard_scroll_target: AsyncScrollNodeStableID,
    pub delta: FloatPoint,
    pub scrolls_to_extent: bool,
    pub snap_selection_intent: SnapSelectionStrategyType,
}

impl Default for KeyStep {
    fn default() -> Self {
        Self {
            keyboard_scroll_target: AsyncScrollNodeStableID {
                node_id: Default::default(),
                kind: libcompositing_rust::scrolling::AsyncScrollNodeKind::Element,
                pseudo_element_type: 0,
            },
            delta: Default::default(),
            scrolls_to_extent: false,
            snap_selection_intent: SnapSelectionStrategyType::EndPositionAndDirection,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyRelease {
    Ignored,
    ReleasedWhileOtherKeysAreHeld,
    ReleasedLastHeldKey,
}

impl Default for KeyboardScrollController {
    fn default() -> Self {
        Self {
            state: KeyboardScrollState {
                generation: 0,
                visual_context_tree_structural_epoch: 0,
                target: None,
                page_scroll_distance: 0.0,
                arrow_scroll_distance: 0.0,
            },
            held_keys: Vec::new(),
        }
    }
}

impl KeyboardScrollController {
    pub fn invalidate_state(&mut self, generation: u64) {
        if generation < self.state.generation {
            return;
        }
        self.state.generation = generation;
        self.state.target = None;
    }

    // Returns whether the state is not older than the one already adopted.
    pub fn adopt_state(&mut self, state: KeyboardScrollState) -> bool {
        if state.generation < self.state.generation {
            return false;
        }
        self.state = state;
        true
    }

    pub fn forget_target(&mut self) {
        self.state.target = None;
    }
    pub fn has_target(&self) -> bool {
        self.state.target.is_some()
    }
    pub fn state_matches_visual_context_tree_epoch(&self, visual_context_tree_structural_epoch: u64) -> bool {
        self.state.visual_context_tree_structural_epoch == visual_context_tree_structural_epoch
    }

    pub fn step_for_key(&self, event: &KeyEvent, async_scroll_tree: &AsyncScrollTree) -> Option<KeyStep> {
        assert!(self.state.target.is_some());
        let modifiers_without_keypad = event.modifiers.0 & !KeyModifier::Mod_Keypad.0;
        let scrolls_to_start = event.key == KeyCode::Key_Home
            || (event.key == KeyCode::Key_Up && modifiers_without_keypad == KeyModifier::Mod_PlatformCtrl.0);
        let scrolls_to_end = event.key == KeyCode::Key_End
            || (event.key == KeyCode::Key_Down && modifiers_without_keypad == KeyModifier::Mod_PlatformCtrl.0);
        let scrolls_to_extent = scrolls_to_start || scrolls_to_end;
        let is_arrow = !scrolls_to_extent
            && matches!(
                event.key,
                KeyCode::Key_Up | KeyCode::Key_Down | KeyCode::Key_Left | KeyCode::Key_Right
            );
        let delta = if scrolls_to_extent {
            async_scroll_tree.device_offset_from_css_pixels(CssPixelPoint {
                x: CssPixels::default(),
                y: if scrolls_to_start {
                    -CssPixels::from_raw(i32::MAX)
                } else {
                    CssPixels::from_raw(i32::MAX)
                },
            })
        } else {
            let mut distance = if is_arrow {
                self.state.arrow_scroll_distance
            } else {
                self.state.page_scroll_distance
            };
            if !distance.is_finite() || distance <= 0.0 {
                return None;
            }
            if matches!(event.key, KeyCode::Key_PageUp | KeyCode::Key_Up | KeyCode::Key_Left)
                || (event.key == KeyCode::Key_Space && !(event.modifiers & KeyModifier::Mod_Shift).is_empty())
            {
                distance = -distance;
            }
            let is_horizontal = matches!(event.key, KeyCode::Key_Left | KeyCode::Key_Right);
            if is_horizontal {
                FloatPoint { x: distance, y: 0.0 }
            } else {
                FloatPoint { x: 0.0, y: distance }
            }
        };

        let mut snap_selection_intent = SnapSelectionStrategyType::EndPositionAndDirection;
        if scrolls_to_extent {
            snap_selection_intent = SnapSelectionStrategyType::EndPosition;
        } else if is_arrow {
            snap_selection_intent = SnapSelectionStrategyType::Direction;
        }

        Some(KeyStep {
            keyboard_scroll_target: self.state.target.unwrap(),
            delta,
            scrolls_to_extent,
            snap_selection_intent,
        })
    }

    pub fn hold_key(&mut self, key: KeyCode) {
        if !self.held_keys.contains(&key) {
            self.held_keys.push(key);
        }
    }

    pub fn release_key(&mut self, event: &KeyEvent) -> KeyRelease {
        if event.repeat {
            return KeyRelease::Ignored;
        }
        let Some(index) = self.held_keys.iter().position(|&key| key == event.key) else {
            return KeyRelease::Ignored;
        };
        self.held_keys.remove(index);
        if self.held_keys.is_empty() {
            return KeyRelease::ReleasedLastHeldKey;
        }
        KeyRelease::ReleasedWhileOtherKeysAreHeld
    }

    // Returns whether any key was held.
    pub fn release_all_keys(&mut self) -> bool {
        if self.held_keys.is_empty() {
            return false;
        }
        self.held_keys.clear();
        true
    }

    pub fn has_held_keys(&self) -> bool {
        !self.held_keys.is_empty()
    }
}
