/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/HistoryOperation.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.history_entry));
    TRY(encoder.encode(parameters.navigation_id));
    TRY(encoder.encode(parameters.history_handling));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::FinalizeCrossDocumentNavigationHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .history_entry = TRY(decoder.decode<Web::HTML::PendingSessionHistoryEntryDescriptor>()),
        .navigation_id = TRY(decoder.decode<Optional<Utf16String>>()),
        .history_handling = TRY(decoder.decode<Web::HTML::HistoryHandlingBehavior>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::CrossDocumentNavigationFinalizationHostState const& state)
{
    TRY(encoder.encode(state.pending_document_is_in_auxiliary_browsing_context_with_opener));
    TRY(encoder.encode(state.pending_document_origin));
    TRY(encoder.encode(state.active_document_origin));
    return {};
}

template<>
ErrorOr<Web::CrossDocumentNavigationFinalizationHostState> IPC::decode(Decoder& decoder)
{
    return Web::CrossDocumentNavigationFinalizationHostState {
        .pending_document_is_in_auxiliary_browsing_context_with_opener = TRY(decoder.decode<bool>()),
        .pending_document_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
        .active_document_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ReconstructedChildNavigation const& navigation)
{
    TRY(encoder.encode(navigation.target_entry));
    TRY(encoder.encode(navigation.navigation_id));
    return {};
}

template<>
ErrorOr<Web::ReconstructedChildNavigation> IPC::decode(Decoder& decoder)
{
    return Web::ReconstructedChildNavigation {
        .target_entry = TRY(decoder.decode<Web::HTML::SessionHistoryEntryDescriptor>()),
        .navigation_id = TRY(decoder.decode<Utf16String>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ReloadHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::ReloadHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::ReloadHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TraverseByDeltaHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.traversable_id));
    TRY(encoder.encode(parameters.delta));
    TRY(encoder.encode(parameters.initiator_to_check));
    TRY(encoder.encode(parameters.initiator_source_snapshot));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::TraverseByDeltaHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::TraverseByDeltaHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .delta = TRY(decoder.decode<i32>()),
        .initiator_to_check = TRY(decoder.decode<Optional<Web::HTML::CrossProcessId>>()),
        .initiator_source_snapshot = TRY(decoder.decode<Optional<Web::InitiatorSourceSnapshot>>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::InitiatorSourceSnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.sandboxing_flags));
    TRY(encoder.encode(snapshot.has_transient_activation));
    return {};
}

template<>
ErrorOr<Web::InitiatorSourceSnapshot> IPC::decode(Decoder& decoder)
{
    return Web::InitiatorSourceSnapshot {
        .sandboxing_flags = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .has_transient_activation = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TraverseToStepHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.traversable_id));
    TRY(encoder.encode(parameters.target_step));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::TraverseToStepHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::TraverseToStepHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .target_step = TRY(decoder.decode<i32>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NavigationAPITraverseHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.key));
    TRY(encoder.encode(parameters.initiator_source_snapshot));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::NavigationAPITraverseHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::NavigationAPITraverseHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .key = TRY(decoder.decode<Utf16String>()),
        .initiator_source_snapshot = TRY(decoder.decode<Optional<Web::InitiatorSourceSnapshot>>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ResumeTraverseHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.target_step));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::ResumeTraverseHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::ResumeTraverseHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .target_step = TRY(decoder.decode<i32>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NavigableCreationHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.parent_navigable_id));
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.initial_history_entry));
    return {};
}

template<>
ErrorOr<Web::NavigableCreationHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::NavigableCreationHistoryOperationParameters {
        .parent_navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .initial_history_entry = TRY(decoder.decode<Web::HTML::PendingSessionHistoryEntryDescriptor>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NavigableDestructionHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.parent_navigable_id));
    TRY(encoder.encode(parameters.parent_document_state_id));
    TRY(encoder.encode(parameters.navigable_id));
    return {};
}

template<>
ErrorOr<Web::NavigableDestructionHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::NavigableDestructionHistoryOperationParameters {
        .parent_navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .parent_document_state_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.target_entry));
    TRY(encoder.encode(parameters.entry_to_replace));
    TRY(encoder.encode(parameters.previous_entry_persisted_state));
    TRY(encoder.encode(parameters.history_handling));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::FinalizeSameDocumentNavigationHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::FinalizeSameDocumentNavigationHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .target_entry = TRY(decoder.decode<Web::HTML::SameDocumentNavigationEntry>()),
        .entry_to_replace = TRY(decoder.decode<Optional<Web::HTML::SessionHistoryEntryIdentity>>()),
        .previous_entry_persisted_state = TRY(decoder.decode<Optional<Web::HTML::SessionHistoryEntryPersistedState>>()),
        .history_handling = TRY(decoder.decode<Web::HTML::HistoryHandlingBehavior>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::CloseTopLevelTraversableHistoryOperationParameters const& parameters)
{
    return encoder.encode(parameters.traversable_id);
}

template<>
ErrorOr<Web::CloseTopLevelTraversableHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::CloseTopLevelTraversableHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::FlushSessionHistoryTraversalQueueOperationParameters const& parameters)
{
    return encoder.encode(parameters.traversable_id);
}

template<>
ErrorOr<Web::FlushSessionHistoryTraversalQueueOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::FlushSessionHistoryTraversalQueueOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}
