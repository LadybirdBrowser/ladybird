/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::selector_parser::RustParsedSelectorList;
use std::sync::Arc;

pub struct ScopeSelectors {
    pub(crate) start: Option<Arc<RustParsedSelectorList>>,
    pub(crate) end: Option<Arc<RustParsedSelectorList>>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ScopeSelectors>();
};
