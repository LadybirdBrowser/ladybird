/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;

// Anonymous layer identities and qualified names belong to the document, not
// this immutable, shareable representation of the names written in a rule.
pub struct LayerNames {
    pub(crate) names: Box<[CssString]>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<LayerNames>();
};
