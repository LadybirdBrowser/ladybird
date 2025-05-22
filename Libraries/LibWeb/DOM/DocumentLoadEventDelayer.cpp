/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>

namespace Web::DOM {

DocumentLoadEventDelayer::DocumentLoadEventDelayer(Document& document)
    : m_document(GC::make_root(document))
{
    m_document->increment_number_of_things_delaying_the_load_event({});
}

DocumentLoadEventDelayer::DocumentLoadEventDelayer(DocumentLoadEventDelayer&& delayer)
    : m_document(move(delayer.m_document))
{
    delayer.m_document = nullptr;
}

DocumentLoadEventDelayer& DocumentLoadEventDelayer::operator=(DocumentLoadEventDelayer&& delayer)
{
    m_document = move(delayer.m_document);
    delayer.m_document = nullptr;

    return *this;
}

DocumentLoadEventDelayer::~DocumentLoadEventDelayer()
{
    if (m_document)
        m_document->decrement_number_of_things_delaying_the_load_event({});
}

}
