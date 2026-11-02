/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PerformanceServerTiming.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::ServerTiming {

// https://w3c.github.io/server-timing/#the-performanceservertiming-interface
class PerformanceServerTiming : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(PerformanceServerTiming, performance_server_timing)

public:
    static GC::Ptr<PerformanceServerTiming> parse_a_server_timing_header_field(StringView field);
};

}
