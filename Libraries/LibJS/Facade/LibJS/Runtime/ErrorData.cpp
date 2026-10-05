/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static JSErrorData const* error_data_to_abi(ErrorData const& error_data)
{
    return reinterpret_cast<JSErrorData const*>(&error_data);
}

TracebackFrameSourceRange const& TracebackFrame::source_range() const
{
    static NeverDestroyed<TracebackFrameSourceRange> source_range_of_a_frame_without_one;
    if (!cached_source_range.has_value())
        return *source_range_of_a_frame_without_one;
    return *cached_source_range;
}

Utf16String ErrorData::stack_string(CompactTraceback compact) const
{
    return owned_utf16_string_from_abi(js_error_data_stack_string(error_data_to_abi(*this), compact == CompactTraceback::Yes));
}

Vector<TracebackFrame, 32> ErrorData::traceback() const
{
    auto const* error_data = error_data_to_abi(*this);
    auto frame_count = js_error_data_traceback_length(error_data);

    Vector<TracebackFrame, 32> traceback;
    traceback.ensure_capacity(frame_count);
    for (size_t index = 0; index < frame_count; ++index) {
        JSTracebackFrame frame;
        js_error_data_traceback_frame(error_data, index, &frame);

        Optional<TracebackFrameSourceRange> source_range;
        if (frame.has_source_range)
            source_range = TracebackFrameSourceRange { Utf16String::from_utf16(utf16_view_from_abi(frame.filename)), { frame.line, frame.column } };
        traceback.unchecked_append({ Utf16String::from_utf16(utf16_view_from_abi(frame.function_name)), move(source_range) });
    }
    return traceback;
}

GC::Ref<ErrorDataCell> ErrorDataCell::capture(VM& vm)
{
    auto* cell = js_error_data_cell_capture(vm_to_abi(vm));
    VERIFY(cell);
    return *cell_from_abi<ErrorDataCell>(cell);
}

}
