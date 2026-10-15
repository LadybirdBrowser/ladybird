/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Function.h>
#include <LibWebCommon/HTML/ApplyHistoryStep.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/ReplicatedNavigableState.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>

namespace Web::HTML {

using OnApplyHistoryStepComplete = GC::Function<void(HistoryStepResult)>;

using OnChangingNavigableHistoryStepJobComplete = GC::Function<void(ChangingNavigableHistoryStepJobDisposition, bool update_only, Optional<CrossProcessId> target_document_id)>;

using OnChangingNavigableContinuationApplied = GC::Function<void(Optional<HostedNavigableState> activated_navigable_state, Optional<SessionHistoryEntryPersistedState> previous_entry_persisted_state)>;

}
