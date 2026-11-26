/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::ui_event::UIEventInit;

/// The dictionary EventModifierInit of UIEvents/EventModifier.idl.
pub(crate) struct EventModifierInit {
    pub(crate) parent: UIEventInit,
    pub(crate) ctrl_key: bool,
    pub(crate) shift_key: bool,
    pub(crate) alt_key: bool,
    pub(crate) meta_key: bool,
    pub(crate) modifier_alt_graph: bool,
    pub(crate) modifier_caps_lock: bool,
    pub(crate) modifier_fn: bool,
    pub(crate) modifier_fn_lock: bool,
    pub(crate) modifier_hyper: bool,
    pub(crate) modifier_num_lock: bool,
    pub(crate) modifier_scroll_lock: bool,
    pub(crate) modifier_super: bool,
    pub(crate) modifier_symbol: bool,
    pub(crate) modifier_symbol_lock: bool,
}
