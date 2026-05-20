/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

struct RustFfiPreloadScannerEntry;

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/parsing.html#speculative-html-parser
class SpeculativeHTMLParser final : public JS::Cell {
    GC_CELL(SpeculativeHTMLParser, JS::Cell);
    GC_DECLARE_ALLOCATOR(SpeculativeHTMLParser);

public:
    static GC::Ref<SpeculativeHTMLParser> create(JS::Realm&, GC::Ref<DOM::Document>, String pending_input, URL::URL base_url);

    virtual ~SpeculativeHTMLParser() override;

    void run();
    void stop();

private:
    SpeculativeHTMLParser(GC::Ref<DOM::Document>, String pending_input, URL::URL base_url);
    virtual void visit_edges(JS::Cell::Visitor&) override;

    void process_preload_scanner_entry(RustFfiPreloadScannerEntry const&);

    GC::Ref<DOM::Document> m_document;
    String m_input;
    URL::URL m_base_url;
    bool m_stopped { false };
};

}
