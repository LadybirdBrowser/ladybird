/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/HTMLDocument.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDocument);

HTMLDocument::HTMLDocument(Page& page, GC::Ref<DOM::EventTarget> relevant_global_event_target, URL::URL const& url)
    : Document(page, relevant_global_event_target, url)
{
}

HTMLDocument::~HTMLDocument() = default;

GC::Ref<HTMLDocument> HTMLDocument::create(Page& page, GC::Ref<DOM::EventTarget> relevant_global_event_target, URL::URL const& url)
{
    auto document = GC::Heap::the().allocate<HTMLDocument>(page, relevant_global_event_target, url);
    document->initialize_document();
    return document;
}

}
