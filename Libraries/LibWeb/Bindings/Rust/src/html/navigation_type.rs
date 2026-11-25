/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationtype
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum NavigationType {
    Push,
    Replace,
    Reload,
    Traverse,
}
