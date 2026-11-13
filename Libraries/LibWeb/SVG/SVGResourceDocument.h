/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/linking.html#processingURL-fetch
// The document that a use element's reference to another document fetches. It's a resource document, not an image: Any
// well-formed XML document will do, whatever its root element, since a use element only looks up an element in it by
// its ID. Every use element of a document that refers to the same URL shares one.
class SVGResourceDocument final : public JS::Cell {
    GC_CELL(SVGResourceDocument, JS::Cell);
    GC_DECLARE_ALLOCATOR(SVGResourceDocument);

public:
    static GC::Ref<SVGResourceDocument> get_or_create(DOM::Document&, URL::URL const&);

    // The parsed document, once the fetch has finished; null until then, and after a failed fetch or parse.
    GC::Ptr<DOM::Document> document() const { return m_resource_document; }

    bool needs_fetching() const { return m_state == State::New; }
    void fetch(NonnullRefPtr<Fetch::Infrastructure::Request>);

    // Calls on_done once the fetch has ended, whether it gave a document or not. If it has already ended, calls it at
    // once and returns no ID.
    Optional<u64> add_callback(GC::Ref<GC::Function<void()>> on_done);
    void remove_callback(u64 id);

private:
    SVGResourceDocument(DOM::Document&, URL::URL);

    virtual void visit_edges(Visitor&) override;

    void finish(GC::Ptr<DOM::Document>);
    void stop();

    enum class State {
        New,
        Fetching,
        Done,
    };

    struct Callback {
        u64 id { 0 };
        GC::Ref<GC::Function<void()>> on_done;
    };

    GC::Ref<DOM::Document> m_document;
    URL::URL m_url;
    State m_state { State::New };
    GC::Ptr<DOM::Document> m_resource_document;
    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Vector<Callback> m_callbacks;
    u64 m_next_callback_id { 1 };
};

}
