/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/HTML/CrossProcessId.h>

namespace Web::HTML {

struct SessionHistoryEntryIdentity {
    CrossProcessId document_state_id;
    Utf16String navigation_api_id;

    bool operator==(SessionHistoryEntryIdentity const&) const = default;
};

}
