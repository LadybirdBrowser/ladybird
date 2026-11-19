/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Parser/ParserScriptingMode.h>

struct RustFfiPreloadScanner;
struct RustFfiPreloadScannerEntry;
struct RustFfiTokenizerHandle;

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/parsing.html#speculative-html-parser
class SpeculativeHTMLParser final : public JS::Cell {
    GC_CELL(SpeculativeHTMLParser, JS::Cell);
    GC_DECLARE_ALLOCATOR(SpeculativeHTMLParser);

public:
    static GC::Ref<SpeculativeHTMLParser> create(GC::Ref<DOM::Document>, ParserScriptingMode);

    // Takes the input of the parser's tokenizer that this hasn't scanned yet. That's the input from where the last run
    // stopped, if the parser hasn't got past there, or else the parser's unparsed input, which a new scan starts on
    // with the given base URL.
    void take_unscanned_input(RustFfiTokenizerHandle*, URL::URL const& base_url);

    void run();
    void stop();

private:
    SpeculativeHTMLParser(GC::Ref<DOM::Document>, ParserScriptingMode);
    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    void process_preload_scanner_entry(RustFfiPreloadScannerEntry const&);

    GC::Ref<DOM::Document> m_document;
    RustFfiPreloadScanner* m_scanner { nullptr };
    URL::URL m_base_url;
    URL::URL m_document_base_url;
    bool m_stopped { false };
};

}
