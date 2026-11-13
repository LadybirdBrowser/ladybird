/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GlobalEventHandlers, https://html.spec.whatwg.org/multipage/webappapis.html#globaleventhandlers

use ak::Utf16String;

unsafe extern "C" {
    fn web_html_is_event_handler_content_attribute(name: usize) -> bool;
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-content-attributes
pub(crate) fn is_event_handler_content_attribute(name: &Utf16String) -> bool {
    // SAFETY: The string is live for the call.
    unsafe { web_html_is_event_handler_content_attribute(name.raw_identity()) }
}
