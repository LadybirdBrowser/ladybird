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
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>

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

    void process_start_tag(HTMLToken const&);
    void process_end_tag(HTMLToken const&);

    GC::Ref<DOM::Document> m_document;
    // m_input must precede m_tokenizer so that m_input.bytes_as_string_view() is valid when the
    // tokenizer's constructor runs in our initializer list.
    String m_input;
    HTMLTokenizer m_tokenizer;
    URL::URL m_base_url;

    u32 m_template_depth { 0 };
    u32 m_foreign_depth { 0 };
};

}
