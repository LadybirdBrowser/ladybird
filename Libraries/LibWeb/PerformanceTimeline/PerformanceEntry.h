/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceEntry.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::PerformanceTimeline {

// https://w3c.github.io/performance-timeline/#dom-performanceentry
// The C++ side of a PerformanceEntry, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src).
class WEB_API PerformanceEntry : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(PerformanceEntry, performance_entry)
};

}
