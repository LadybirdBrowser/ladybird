/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossProcessId.h>

namespace Web::HTML {

struct SessionHistoryEntryIdentity {
    CrossProcessId document_state_id;
    Utf16String navigation_api_id;

    bool operator==(SessionHistoryEntryIdentity const&) const = default;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryEntryIdentity const&);

template<>
WEB_API ErrorOr<Web::HTML::SessionHistoryEntryIdentity> decode(Decoder&);

}
