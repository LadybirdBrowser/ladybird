/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

// Whether the user agent itself supplied the URL of the navigation that created a session history entry — the
// address bar, a bookmark, the new tab page, the command line, a WebDriver client — rather than a page. That's a
// navigation with no source document: A URL the page chose has the page as its source, even when the user opened it
// through the browser's UI (a link's context menu, a Ctrl-click), as section 4.3 of Fetch Metadata asks for.
// A document state records it, since a traversal or reload can't tell from its own user navigation involvement: Back
// and Forward are "browser UI" even when the entry came from a link.
// https://w3c.github.io/webappsec-fetch-metadata/#directly-user-initiated
enum class UserAgentInitiated {
    No,
    Yes,
};

}
