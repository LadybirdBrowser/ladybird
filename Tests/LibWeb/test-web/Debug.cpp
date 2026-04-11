/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Debug.h"
#include "TestWeb.h"

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>

namespace TestWeb {

static void maybe_attach_lldb_to_process(pid_t pid)
{
    if (pid <= 0)
        return;

    Vector<ByteString> arguments;
    arguments.append("-p"sv);
    arguments.append(ByteString::number(pid));

    auto process_or_error = Core::Process::spawn({
        .executable = "lldb"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    });

    if (process_or_error.is_error()) {
        warnln("Failed to spawn lldb: {}", process_or_error.error());
        return;
    }

    Core::Process lldb = process_or_error.release_value();
    if (auto wait_result = lldb.wait_for_termination(); wait_result.is_error())
        warnln("Failed waiting for lldb: {}", wait_result.error());
}

static void maybe_attach_gdb_to_process(pid_t pid)
{
    if (pid <= 0)
        return;

    Vector<ByteString> arguments;
    arguments.append("-q"sv);
    arguments.append("-p"sv);
    arguments.append(ByteString::number(pid));

    auto process_or_error = Core::Process::spawn({
        .executable = "gdb"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    });

    if (process_or_error.is_error()) {
        warnln("Failed to spawn gdb: {}", process_or_error.error());
        return;
    }

    Core::Process gdb = process_or_error.release_value();
    if (auto wait_result = gdb.wait_for_termination(); wait_result.is_error())
        warnln("Failed waiting for gdb: {}", wait_result.error());
}

void maybe_attach_on_fail_fast_timeout(pid_t pid)
{
    if (pid <= 0
        || !Core::System::isatty(STDIN_FILENO).value_or(false)
        || !Core::System::isatty(STDOUT_FILENO).value_or(false))
        return;

    outln("Fail-fast timeout in WebContent pid {}.", pid);
    outln("You may attach a debugger now (test-web will wait)."sv);
    outln("- Press Enter to continue shutdown + exit"sv);
    outln("- Type 'gdb' then Enter to attach with gdb first"sv);
    outln("- Type 'lldb' then Enter to attach with lldb first"sv);
    MUST(Core::System::write(1, "> "sv.bytes()));

    auto standard_input_or_error = Core::File::standard_input();
    if (standard_input_or_error.is_error())
        return;

    Array<u8, 64> input_buffer {};
    auto buffered_standard_input_or_error = Core::InputBufferedFile::create(standard_input_or_error.release_value());
    if (buffered_standard_input_or_error.is_error())
        return;

    auto& buffered_standard_input = buffered_standard_input_or_error.value();
    auto response_or_error = buffered_standard_input->read_line(input_buffer);
    if (response_or_error.is_error())
        return;

    ByteString response { response_or_error.value() };
    response = response.trim_whitespace();
    if (response.equals_ignoring_ascii_case("gdb"sv)) {
        maybe_attach_gdb_to_process(pid);
        return;
    }
    if (response.equals_ignoring_ascii_case("lldb"sv))
        maybe_attach_lldb_to_process(pid);
}

}
