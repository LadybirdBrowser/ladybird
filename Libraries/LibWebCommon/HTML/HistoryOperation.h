/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/ApplyHistoryStep.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/HistoryHandlingBehavior.h>
#include <LibWebCommon/HTML/SameDocumentNavigationEntry.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/HTML/SessionHistoryEntryIdentity.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>

namespace Web {

struct FinalizeCrossDocumentNavigationHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::PendingSessionHistoryEntryDescriptor history_entry;
    Optional<Utf16String> navigation_id;
    HTML::HistoryHandlingBehavior history_handling;
    HTML::UserNavigationInvolvement user_involvement;
    Optional<Web::HTML::EnvironmentId> environment_id;
    Optional<HTML::CrossProcessId> document_id;
};

struct ReconstructedChildNavigation {
    HTML::SessionHistoryEntryDescriptor target_entry;
    Utf16String navigation_id;
};

using HistoryOperationReadyResult = Variant<Empty, HTML::HistoryStepResult>;

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
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ReconstructedChildNavigation const&);
template<>
WEBCOMMON_API ErrorOr<Web::ReconstructedChildNavigation> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ReloadHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::ReloadHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::InitiatorSourceSnapshot const&);
template<>
WEBCOMMON_API ErrorOr<Web::InitiatorSourceSnapshot> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TraverseByDeltaHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::TraverseByDeltaHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TraverseToStepHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::TraverseToStepHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::NavigationAPITraverseHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::NavigationAPITraverseHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ResumeTraverseHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::ResumeTraverseHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::NavigableCreationHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::NavigableCreationHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::NavigableDestructionHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::NavigableDestructionHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::FinalizeSameDocumentNavigationHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::CloseTopLevelTraversableHistoryOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::CloseTopLevelTraversableHistoryOperationParameters> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::FlushSessionHistoryTraversalQueueOperationParameters const&);
template<>
WEBCOMMON_API ErrorOr<Web::FlushSessionHistoryTraversalQueueOperationParameters> decode(Decoder&);

}
