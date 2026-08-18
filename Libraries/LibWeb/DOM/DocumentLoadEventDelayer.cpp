/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    if (m_document) {
        if (m_reason == DocumentLoadEventDelayerReason::StyleSheetRequest)
            m_document->decrement_number_of_pending_style_sheet_requests({});
        m_document->decrement_number_of_things_delaying_the_load_event({});
    }
}

}
