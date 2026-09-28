/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-history-policy-container
// A document state's history policy container is a policy container or null, where "client" is represented by this tag.
enum class DocumentStateClient {
    Tag,
};

}
