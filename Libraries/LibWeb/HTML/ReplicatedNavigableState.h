/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SessionHistoryEntryIdentity.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

struct ReplicatedContainerState {
    // Whether the container is in its node document's tree rather than a shadow tree, which makes the navigable a
    // document-tree child navigable of that document.
    bool is_in_document_tree { false };

    // https://html.spec.whatwg.org/multipage/browsers.html#iframe-sandboxing-flag-set
    SandboxingFlagSet iframe_sandboxing_flag_set {};

    // https://html.spec.whatwg.org/multipage/browsers.html#active-sandboxing-flag-set, of the container's node document.
    SandboxingFlagSet document_active_sandboxing_flag_set {};

    // The container's local name, which navigate makes its request's destination and initiator type.
    Optional<Utf16FlyString> local_name;

    // https://html.spec.whatwg.org/multipage/browsers.html#determining-the-iframe-element-referrer-policy
    ReferrerPolicy::ReferrerPolicy iframe_referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
};

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

    bool active_document_is_completely_loaded { false };
    bool is_closing { false };

    ReplicatedContainerState container;

    bool delays_the_load_event_of_its_container { false };
    bool has_session_history_entry_and_ready_for_navigation { false };

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
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ReplicatedContainerState const&);
template<>
WEB_API ErrorOr<Web::HTML::ReplicatedContainerState> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ReplicatedNavigableState const&);
template<>
WEB_API ErrorOr<Web::HTML::ReplicatedNavigableState> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::RemoteNavigableDescriptor const&);
template<>
WEB_API ErrorOr<Web::HTML::RemoteNavigableDescriptor> decode(Decoder&);

}
