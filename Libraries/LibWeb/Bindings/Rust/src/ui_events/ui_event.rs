/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::bindings::conversions::CppInterface;
use crate::dom::event::EventInit;

/// The dictionary UIEventInit of UIEvents/UIEvent.idl.
pub(crate) struct UIEventInit {
    pub(crate) parent: EventInit,
    pub(crate) view: Option<CppInterface>,
    pub(crate) detail: i32,
}
