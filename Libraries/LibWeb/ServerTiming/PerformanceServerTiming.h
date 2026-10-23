/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::ServerTiming {

// https://w3c.github.io/server-timing/#the-performanceservertiming-interface
class PerformanceServerTiming final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(PerformanceServerTiming, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(PerformanceServerTiming);

public:
    static GC::Ptr<PerformanceServerTiming> parse_a_server_timing_header_field(StringView field);

    virtual ~PerformanceServerTiming() override;

    String const& name() const { return m_metric_name; }
    HighResolutionTime::DOMHighResTimeStamp duration() const;
    String description() const;

private:
    explicit PerformanceServerTiming(String metric_name);

    // https://w3c.github.io/server-timing/#dfn-metric-name
    String m_metric_name;

    // https://w3c.github.io/server-timing/#dfn-params
    OrderedHashMap<String, String> m_params;
};

}
