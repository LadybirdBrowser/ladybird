/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

static SourceRange dummy_source_range { SourceCode::create({}, {}), {}, {} };

SourceRange const& TracebackFrame::source_range() const
{
    if (!cached_source_range.has_value())
        return dummy_source_range;
    return *cached_source_range;
}

ErrorData::ErrorData(VM& vm)
{
    populate_stack(vm);
}

void ErrorData::visit_edges(Cell::Visitor& visitor)
{
    visitor.visit(m_cached_string);
}

void ErrorData::populate_stack(VM& vm)
{
    auto stack_trace = vm.stack_trace();
    m_traceback.ensure_capacity(stack_trace.size());
    for (auto& element : stack_trace) {
        auto* context = element.execution_context;
        m_traceback.append({
            .function_name = context->function ? context->function->name_for_call_stack() : ""_utf16,
            .cached_source_range = move(element.source_range),
        });
    }
}

Utf16String ErrorData::stack_string(CompactTraceback compact) const
{
    if (m_traceback.is_empty())
        return {};

    StringBuilder stack_string_builder(StringBuilder::Mode::UTF16);

    // Note: We roughly follow V8's formatting
    auto append_frame = [&](TracebackFrame const& frame) {
        auto const& function_name = frame.function_name;
        auto const& source_range = frame.source_range();
        // Note: Since we don't know whether we have a valid SourceRange here we just check for some default values.
        if (!source_range.filename().is_empty() || source_range.start.offset != 0 || source_range.end.offset != 0) {

            if (function_name.is_empty())
                stack_string_builder.appendff("    at {}:{}:{}\n", source_range.filename(), source_range.start.line, source_range.start.column);
            else
                stack_string_builder.appendff("    at {} ({}:{}:{})\n", function_name, source_range.filename(), source_range.start.line, source_range.start.column);
        } else {
            stack_string_builder.appendff("    at {}\n", function_name.is_empty() ? "<unknown>"_utf16 : function_name);
        }
    };

    auto is_same_frame = [](TracebackFrame const& a, TracebackFrame const& b) {
        if (a.function_name.is_empty() && b.function_name.is_empty()) {
            auto const& source_range_a = a.source_range();
            auto const& source_range_b = b.source_range();
            return source_range_a.filename() == source_range_b.filename() && source_range_a.start.line == source_range_b.start.line;
        }
        return a.function_name == b.function_name;
    };

    // Note: We don't want to capture the global execution context, so we omit the last frame
    // Note: The error's name and message get prepended by Error.prototype.stack
    unsigned repetitions = 0;
    size_t used_frames = m_traceback.size() - 1;
    for (size_t i = 0; i < used_frames; ++i) {
        auto const& frame = m_traceback[i];
        if (compact == CompactTraceback::Yes && i + 1 < used_frames) {
            auto const& next_traceback_frame = m_traceback[i + 1];
            if (is_same_frame(frame, next_traceback_frame)) {
                repetitions++;
                continue;
            }
        }
        if (repetitions > 4) {
            // If more than 5 (1 + >4) consecutive function calls with the same name, print
            // the name only once and show the number of repetitions instead. This prevents
            // printing ridiculously large call stacks of recursive functions.
            append_frame(frame);
            stack_string_builder.appendff("    {} more calls\n", repetitions);
        } else {
            for (size_t j = 0; j < repetitions + 1; j++)
                append_frame(frame);
        }
        repetitions = 0;
    }
    for (size_t j = 0; j < repetitions; j++)
        append_frame(m_traceback[used_frames - 1]);

    return stack_string_builder.to_utf16_string();
}

}
