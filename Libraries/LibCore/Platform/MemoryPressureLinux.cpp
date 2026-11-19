/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <LibCore/File.h>
#include <LibCore/Platform/MemoryPressure.h>

namespace Core::Platform {

// Linux tells how much memory it could hand out without swapping, but not how short of it the system is. It is about
// to run out once little of that is left: a twentieth of all memory, but at least 200 MiB, and on a machine with much
// memory, no more than 1 GiB.
static constexpr u64 critical_available_memory_percent = 5;
static constexpr u64 minimum_critical_available_memory_in_kib = 200 * MiB / KiB;
static constexpr u64 maximum_critical_available_memory_in_kib = 1 * GiB / KiB;

MemoryPressure system_memory_pressure()
{
    auto file = Core::File::open("/proc/meminfo"sv, Core::File::OpenMode::Read);
    if (file.is_error())
        return MemoryPressure::Normal;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return MemoryPressure::Normal;

    Optional<u64> total_in_kib;
    Optional<u64> available_in_kib;
    for (auto line : StringView { contents.value() }.lines()) {
        GenericLexer lexer { line };
        auto name = lexer.consume_until(':');
        lexer.consume_specific(':');
        lexer.ignore_while(is_ascii_space);
        auto value = lexer.consume_while(is_ascii_digit).to_number<u64>();
        if (name == "MemTotal"sv)
            total_in_kib = value;
        else if (name == "MemAvailable"sv)
            available_in_kib = value;
    }

    // NB: Kernels older than 3.14 don't report the available memory.
    if (!total_in_kib.has_value() || !available_in_kib.has_value() || *total_in_kib == 0)
        return MemoryPressure::Normal;

    auto critical_available_memory_in_kib = clamp(*total_in_kib / 100 * critical_available_memory_percent, minimum_critical_available_memory_in_kib, maximum_critical_available_memory_in_kib);
    if (*available_in_kib <= critical_available_memory_in_kib)
        return MemoryPressure::Critical;
    return MemoryPressure::Normal;
}

}
