/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web {

struct FinalizeCrossDocumentNavigationHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::PendingSessionHistoryEntryDescriptor history_entry;
    Optional<Utf16String> navigation_id;
    HTML::HistoryHandlingBehavior history_handling;
    HTML::UserNavigationInvolvement user_involvement;
};

struct CrossDocumentNavigationFinalizationHostState {
    bool pending_document_is_in_auxiliary_browsing_context_with_opener { false };
    Optional<URL::Origin> pending_document_origin;
    Optional<URL::Origin> active_document_origin;
};

struct ReconstructedChildNavigation {
    HTML::SessionHistoryEntryDescriptor target_entry;
    Utf16String navigation_id;
};

using HistoryOperationReadyResult = Variant<
    Empty,
    HTML::HistoryStepResult,
    HTML::CrossProcessId,
    CrossDocumentNavigationFinalizationHostState>;

struct ReloadHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::UserNavigationInvolvement user_involvement;
};

struct InitiatorSourceSnapshot {
    HTML::SandboxingFlagSet sandboxing_flags {};
    bool has_transient_activation { false };
};

struct TraverseByDeltaHistoryOperationParameters {
    i32 delta;
    Optional<HTML::CrossProcessId> initiator_to_check;
    Optional<InitiatorSourceSnapshot> initiator_source_snapshot;
    HTML::UserNavigationInvolvement user_involvement;
};

struct TraverseToStepHistoryOperationParameters {
    i32 target_step;
    HTML::UserNavigationInvolvement user_involvement;
};

struct NavigationAPITraverseHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    Utf16String key;
    Optional<InitiatorSourceSnapshot> initiator_source_snapshot;
    HTML::UserNavigationInvolvement user_involvement;
};

struct ResumeTraverseHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    i32 target_step;
    HTML::UserNavigationInvolvement user_involvement;
};

struct NavigableCreationHistoryOperationParameters {
    HTML::CrossProcessId parent_navigable_id;
    HTML::CrossProcessId navigable_id;
    HTML::PendingSessionHistoryEntryDescriptor initial_history_entry;
};

struct NavigableDestructionHistoryOperationParameters {
    HTML::CrossProcessId parent_navigable_id;
    HTML::CrossProcessId parent_document_state_id;
    HTML::CrossProcessId navigable_id;
};

struct FinalizeSameDocumentNavigationHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::SameDocumentNavigationEntry target_entry;
    Optional<HTML::SessionHistoryEntryIdentity> entry_to_replace;
    Optional<HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state;
    HTML::HistoryHandlingBehavior history_handling;
    HTML::UserNavigationInvolvement user_involvement;
};

struct CloseTopLevelTraversableHistoryOperationParameters {
};

struct FlushSessionHistoryTraversalQueueOperationParameters {
};

using HistoryOperationParameters = Variant<
    FinalizeCrossDocumentNavigationHistoryOperationParameters,
    ReloadHistoryOperationParameters,
    TraverseByDeltaHistoryOperationParameters,
    TraverseToStepHistoryOperationParameters,
    NavigationAPITraverseHistoryOperationParameters,
    ResumeTraverseHistoryOperationParameters,
    NavigableCreationHistoryOperationParameters,
    NavigableDestructionHistoryOperationParameters,
    FinalizeSameDocumentNavigationHistoryOperationParameters,
    CloseTopLevelTraversableHistoryOperationParameters,
    FlushSessionHistoryTraversalQueueOperationParameters>;

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::CrossDocumentNavigationFinalizationHostState const&);
template<>
WEB_API ErrorOr<Web::CrossDocumentNavigationFinalizationHostState> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ReconstructedChildNavigation const&);
template<>
WEB_API ErrorOr<Web::ReconstructedChildNavigation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ReloadHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::ReloadHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::InitiatorSourceSnapshot const&);
template<>
WEB_API ErrorOr<Web::InitiatorSourceSnapshot> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::TraverseByDeltaHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::TraverseByDeltaHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::TraverseToStepHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::TraverseToStepHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::NavigationAPITraverseHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::NavigationAPITraverseHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ResumeTraverseHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::ResumeTraverseHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::NavigableCreationHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::NavigableCreationHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::NavigableDestructionHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::NavigableDestructionHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::FinalizeSameDocumentNavigationHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::CloseTopLevelTraversableHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::CloseTopLevelTraversableHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::FlushSessionHistoryTraversalQueueOperationParameters const&);
template<>
WEB_API ErrorOr<Web::FlushSessionHistoryTraversalQueueOperationParameters> decode(Decoder&);

}
