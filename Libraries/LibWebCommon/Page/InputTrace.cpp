/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/InputTrace.h>

namespace Web {

// A test that never takes its trace must not grow it without bound.
static constexpr size_t maximum_input_trace_record_count = 10'000;

StringView input_trace_stage_name(InputTraceStage stage)
{
    switch (stage) {
    case InputTraceStage::Compositor:
        return "compositor"sv;
    case InputTraceStage::CompositorForMainThread:
        return "compositor-for-main-thread"sv;
    case InputTraceStage::WebContent:
        return "web-content"sv;
    }
    VERIFY_NOT_REACHED();
}

void InputTraceRecorder::record(InputTraceRecord record)
{
    if (!m_records.has_value() || m_records->size() >= maximum_input_trace_record_count)
        return;
    m_records->append(move(record));
}

Vector<InputTraceRecord> InputTraceRecorder::take()
{
    if (!m_records.has_value())
        return {};
    return m_records.release_value();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::InputTraceRecord const& record)
{
    TRY(encoder.encode(record.input_event_id));
    TRY(encoder.encode(record.stage));
    TRY(encoder.encode(record.decision));
    TRY(encoder.encode(record.scroller));
    TRY(encoder.encode(record.dom_target));
    return {};
}

template<>
ErrorOr<Web::InputTraceRecord> decode(Decoder& decoder)
{
    return Web::InputTraceRecord {
        .input_event_id = TRY(decoder.decode<u64>()),
        .stage = TRY(decoder.decode<Web::InputTraceStage>()),
        .decision = TRY(decoder.decode<Web::InputRoutingDecision>()),
        .scroller = TRY(decoder.decode<Optional<Web::AsyncScrollNodeStableID>>()),
        .dom_target = TRY(decoder.decode<Optional<Web::UniqueNodeID>>()),
    };
}

}
