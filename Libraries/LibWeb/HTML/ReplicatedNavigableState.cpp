/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ReplicatedContainerState const& state)
{
    TRY(encoder.encode(state.is_in_document_tree));
    TRY(encoder.encode(state.iframe_sandboxing_flag_set));
    TRY(encoder.encode(state.document_active_sandboxing_flag_set));
    TRY(encoder.encode(state.local_name));
    TRY(encoder.encode(state.iframe_referrer_policy));
    return {};
}

template<>
ErrorOr<Web::HTML::ReplicatedContainerState> decode(Decoder& decoder)
{
    return Web::HTML::ReplicatedContainerState {
        .is_in_document_tree = TRY(decoder.decode<bool>()),
        .iframe_sandboxing_flag_set = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .document_active_sandboxing_flag_set = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .local_name = TRY(decoder.decode<Optional<Utf16FlyString>>()),
        .iframe_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ReplicatedNavigableState const& state)
{
    TRY(encoder.encode(state.target_name));
    TRY(encoder.encode(state.active_document_url));
    TRY(encoder.encode(state.active_document_origin));
    TRY(encoder.encode(state.active_document_is_fully_active));
    TRY(encoder.encode(state.active_session_history_entry_identity));
    TRY(encoder.encode(state.top_level_creation_url));
    TRY(encoder.encode(state.top_level_origin));
    TRY(encoder.encode(state.has_cross_site_ancestor));
    TRY(encoder.encode(state.opener_policy));
    TRY(encoder.encode(state.active_browsing_context_is_auxiliary));
    TRY(encoder.encode(state.opener_navigable_id));
    TRY(encoder.encode(state.active_document_is_completely_loaded));
    TRY(encoder.encode(state.is_closing));
    TRY(encoder.encode(state.container));
    TRY(encoder.encode(state.delays_the_load_event_of_its_container));
    TRY(encoder.encode(state.has_session_history_entry_and_ready_for_navigation));
    TRY(encoder.encode(state.compositor_context_id));
    return {};
}

template<>
ErrorOr<Web::HTML::ReplicatedNavigableState> decode(Decoder& decoder)
{
    return Web::HTML::ReplicatedNavigableState {
        .target_name = TRY(decoder.decode<Utf16String>()),
        .active_document_url = TRY(decoder.decode<URL::URL>()),
        .active_document_origin = TRY(decoder.decode<URL::Origin>()),
        .active_document_is_fully_active = TRY(decoder.decode<bool>()),
        .active_session_history_entry_identity = TRY(decoder.decode<Web::HTML::SessionHistoryEntryIdentity>()),
        .top_level_creation_url = TRY(decoder.decode<URL::URL>()),
        .top_level_origin = TRY(decoder.decode<URL::Origin>()),
        .has_cross_site_ancestor = TRY(decoder.decode<bool>()),
        .opener_policy = TRY(decoder.decode<Web::HTML::OpenerPolicy>()),
        .active_browsing_context_is_auxiliary = TRY(decoder.decode<bool>()),
        .opener_navigable_id = TRY(decoder.decode<Optional<Web::HTML::CrossProcessId>>()),
        .active_document_is_completely_loaded = TRY(decoder.decode<bool>()),
        .is_closing = TRY(decoder.decode<bool>()),
        .container = TRY(decoder.decode<Web::HTML::ReplicatedContainerState>()),
        .delays_the_load_event_of_its_container = TRY(decoder.decode<bool>()),
        .has_session_history_entry_and_ready_for_navigation = TRY(decoder.decode<bool>()),
        .compositor_context_id = TRY(decoder.decode<Optional<Web::Compositor::CompositorContextId>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::RemoteNavigableDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.id));
    TRY(encoder.encode(descriptor.parent_id));
    TRY(encoder.encode(descriptor.replicated_state));
    return {};
}

template<>
ErrorOr<Web::HTML::RemoteNavigableDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::RemoteNavigableDescriptor {
        .id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .parent_id = TRY(decoder.decode<Optional<Web::HTML::CrossProcessId>>()),
        .replicated_state = TRY(decoder.decode<Web::HTML::ReplicatedNavigableState>()),
    };
}

}
