/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLDocument.h>

namespace Web::HTML {

HTMLDocument::HTMLDocument(DOM::RustMadePrefix prefix, Page& page, GC::Ref<DOM::EventTarget> relevant_global_event_target, URL::URL const& url)
    : Document(prefix, page, relevant_global_event_target, url)
{
}

HTMLDocument::~HTMLDocument() = default;

GC::Ref<HTMLDocument> HTMLDocument::create(Page& page, GC::Ref<DOM::EventTarget> relevant_global_event_target, URL::URL const& url)
{
    auto document = allocate_document<HTMLDocument>(page, relevant_global_event_target, url);
    document->initialize_document();
    return document;
}

}
