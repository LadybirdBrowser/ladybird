/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/ServerTiming/PerformanceServerTiming.h>

namespace Web::ServerTiming {

extern "C" {

// Defined in Bindings/Rust/src/server_timing/performance_server_timing.rs.
void* web_performance_server_timing_parse_a_server_timing_header_field(JSVM*, ReadonlyBytes field);
}

GC::Ptr<PerformanceServerTiming> PerformanceServerTiming::parse_a_server_timing_header_field(StringView field)
{
    return static_cast<PerformanceServerTiming*>(web_performance_server_timing_parse_a_server_timing_header_field(JS::HostABI::vm_to_abi(JS::VM::the()), field.bytes()));
}

}
