/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>
#include <LibWebCommon/Page/InputRoutingDecision.h>

namespace Web {

enum class InputTraceStage : u8 {
    // The compositor, as it takes an input event from the UI.
    Compositor,
    // The compositor, as WebContent's main thread asks it to scroll for an input event.
    CompositorForMainThread,
    WebContent,
};

WEBCOMMON_API StringView input_trace_stage_name(InputTraceStage);

struct InputTraceRecord {
    // The id the UI gave the input event, or 0 for input that did not come from the UI.
    u64 input_event_id { 0 };
    InputTraceStage stage { InputTraceStage::WebContent };
    InputRoutingDecision decision { InputRoutingDecision::NoAnswerFromCompositor };
    Optional<AsyncScrollNodeStableID> scroller {};
    Optional<UniqueNodeID> dom_target {};
};

// Collects the routing decisions of input events while a test asks for them.
class WEBCOMMON_API InputTraceRecorder {
public:
    void begin() { m_records = Vector<InputTraceRecord> {}; }
    bool is_recording() const { return m_records.has_value(); }
    void record(InputTraceRecord);
    // Returns what was recorded since the trace began, and stops recording.
    Vector<InputTraceRecord> take();

private:
    Optional<Vector<InputTraceRecord>> m_records;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::InputTraceRecord const&);
template<>
WEBCOMMON_API ErrorOr<Web::InputTraceRecord> decode(Decoder&);

}
