/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;

pub struct NamespaceRuleData {
    pub(crate) prefix: CssString,
    pub(crate) uri: CssString,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<NamespaceRuleData>();
};
