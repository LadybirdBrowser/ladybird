/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::selector_parser::RustParsedSelectorList;

pub struct ScopeSelectors {
    pub(crate) start: Option<RustParsedSelectorList>,
    pub(crate) end: Option<RustParsedSelectorList>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ScopeSelectors>();
};
