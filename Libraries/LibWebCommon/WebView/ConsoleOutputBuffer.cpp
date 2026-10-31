/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebCommon/WebView/ConsoleOutputBuffer.h>

namespace WebView {

static constexpr size_t overhead = 64;

static size_t estimated_size(JsonValue const& value)
{
    if (value.is_string())
        return value.as_string().bytes().size();

    size_t size = overhead;
    if (value.is_array()) {
        value.as_array().for_each([&](JsonValue const& item) { size += estimated_size(item); });
    } else if (value.is_object()) {
        value.as_object().for_each_member([&](String const& name, JsonValue const& member) {
            size += name.bytes().size() + estimated_size(member);
        });
    }
    return size;
}

// A rough count of the bytes that a message holds: its strings, plus a little for everything else.
static size_t estimated_size(ConsoleOutput const& output)
{
    size_t size = overhead;
    auto add_frames = [&](Vector<StackFrame> const& frames) {
        for (auto const& frame : frames)
            size += overhead + frame.function.map([](auto const& name) { return name.bytes().size(); }).value_or(0) + frame.file.map([](auto const& file) { return file.bytes().size(); }).value_or(0);
    };

    output.output.visit(
        [&](ConsoleLog const& log) {
            for (auto const& argument : log.arguments)
                size += estimated_size(argument);
            if (log.stacktrace.has_value())
                add_frames(*log.stacktrace);
        },
        [&](ConsoleError const& error) {
            size += error.name.bytes().size() + error.message.bytes().size();
            add_frames(error.trace);
        },
        [&](ConsoleTrace const& trace) {
            size += trace.label.bytes().size();
            add_frames(trace.stack);
        },
        [](ConsoleGroupEnd const&) { });
    return size;
}

void ConsoleOutputBuffer::append(ConsoleOutput output)
{
    auto size = estimated_size(output);
    m_entries.append(Entry { move(output), size });
    ++m_message_count;
    m_size += size;

    // The latest message always stays, however big it is.
    while (m_message_count > 1 && (m_message_count > MAX_MESSAGES || m_size > MAX_SIZE)) {
        auto oldest = m_entries.take_first();
        --m_message_count;
        m_size -= oldest.size;
    }
}

void ConsoleOutputBuffer::for_each(Function<void(ConsoleOutput const&)> const& callback) const
{
    for (auto const& entry : m_entries)
        callback(entry.output);
}

}
