/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>

namespace Web::DOM {

DocumentLoadEventDelayer::DocumentLoadEventDelayer(Document& document, DocumentLoadEventDelayerReason reason)
    : m_document(document)
    , m_reason(reason)
{
    m_document->increment_number_of_things_delaying_the_load_event({});
    if (m_reason == DocumentLoadEventDelayerReason::StyleSheetRequest)
        m_document->increment_number_of_pending_style_sheet_requests({});
}

DocumentLoadEventDelayer::DocumentLoadEventDelayer(DocumentLoadEventDelayer&& delayer)
    : m_document(move(delayer.m_document))
    , m_reason(delayer.m_reason)
{
    delayer.m_document = nullptr;
}

DocumentLoadEventDelayer& DocumentLoadEventDelayer::operator=(DocumentLoadEventDelayer&& delayer)
{
    if (this == &delayer)
        return *this;

    if (m_document) {
        if (m_reason == DocumentLoadEventDelayerReason::StyleSheetRequest)
            m_document->decrement_number_of_pending_style_sheet_requests({});
        m_document->decrement_number_of_things_delaying_the_load_event({});
    }

    m_document = move(delayer.m_document);
    m_reason = delayer.m_reason;
    delayer.m_document = nullptr;

    return *this;
}

DocumentLoadEventDelayer::~DocumentLoadEventDelayer()
{
    if (!m_document)
        return;
    auto release = [](GC::Weak<Document> const& document, DocumentLoadEventDelayerReason reason) {
        if (!document || document->has_been_destroyed())
            return;
        if (reason == DocumentLoadEventDelayerReason::StyleSheetRequest)
            document->decrement_number_of_pending_style_sheet_requests({});
        document->decrement_number_of_things_delaying_the_load_event({});
    };
    // NB: Telling the document may allocate, see Document::schedule_html_parser_end_check(), which no destructor may
    //     do while a collection is in progress, as a collection that moves cells destroys the cells that died before
    //     it is over. Tell it once the collection is over.
    auto& heap = m_document->heap();
    if (heap.is_marking_for_collection()) {
        heap.enqueue_post_gc_task([release, document = m_document, reason = m_reason] {
            release(document, reason);
        });
        return;
    }
    release(m_document, m_reason);
}

}
