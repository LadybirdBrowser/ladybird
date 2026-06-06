/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/XMLDocument.h>
#include <LibWeb/DOM/XMLDocument.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(XMLDocument);

GC::Ref<XMLDocument> XMLDocument::create(Page& page, GC::Ref<EventTarget> relevant_global_event_target, URL::URL const& url)
{
    auto document = GC::Heap::the().allocate<XMLDocument>(page, relevant_global_event_target, url);
    document->initialize_document();
    return document;
}

XMLDocument::XMLDocument(Page& page, GC::Ref<EventTarget> relevant_global_event_target, URL::URL const& url)
    : Document(page, relevant_global_event_target, url)
{
}

}
