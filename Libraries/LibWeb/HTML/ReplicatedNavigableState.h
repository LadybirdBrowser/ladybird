/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/SessionHistoryEntryIdentity.h>

namespace Web::HTML {

struct ReplicatedNavigableState {
    Utf16String target_name;
    URL::URL active_document_url;
    URL::Origin active_document_origin;
    bool active_document_is_fully_active { false };
    SessionHistoryEntryIdentity active_session_history_entry_identity;
    URL::URL top_level_creation_url;
    URL::Origin top_level_origin;
    bool has_cross_site_ancestor { false };

    OpenerPolicy opener_policy;

    Optional<Compositor::CompositorContextId> compositor_context_id;
};

struct RemoteNavigableDescriptor {
    CrossProcessId id;
    Optional<CrossProcessId> parent_id;
    ReplicatedNavigableState replicated_state;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ReplicatedNavigableState const&);
template<>
WEB_API ErrorOr<Web::HTML::ReplicatedNavigableState> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::RemoteNavigableDescriptor const&);
template<>
WEB_API ErrorOr<Web::HTML::RemoteNavigableDescriptor> decode(Decoder&);

}
