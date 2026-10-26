/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// https://dom.spec.whatwg.org/#dictdef-eventinit
pub(crate) struct EventInit {
    pub(crate) bubbles: bool,
    pub(crate) cancelable: bool,
    pub(crate) composed: bool,
}
