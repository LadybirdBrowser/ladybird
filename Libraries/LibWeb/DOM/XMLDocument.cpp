/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/XMLDocument.h>

namespace Web::DOM {

GC::Ref<XMLDocument> XMLDocument::create(Page& page, GC::Ref<EventTarget> relevant_global_event_target, URL::URL const& url)
{
    auto document = allocate_document<XMLDocument>(page, relevant_global_event_target, url);
    document->initialize_document();
    return document;
}

XMLDocument::XMLDocument(RustMadePrefix prefix, Page& page, GC::Ref<EventTarget> relevant_global_event_target, URL::URL const& url)
    : Document(prefix, page, relevant_global_event_target, url)
{
}

}
