/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Page/PageHoverLink.h>

namespace Web {

Optional<URL::URL> PageHoverLink::current() const
{
    MutexLocker locker(m_mutex);
    return m_current;
}

bool PageHoverLink::has_link() const
{
    MutexLocker locker(m_mutex);
    return m_current.has_value();
}

void PageHoverLink::request(Optional<URL::URL> url)
{
    // NB: A blob URL's entry is counted without atomics, and a client only shows the URL, so none crosses threads here.
    if (url.has_value())
        url->set_blob_url_entry({});

    MutexLocker locker(m_mutex);
    if (m_current == url)
        return;
    m_current = move(url);
    if (m_send)
        m_send(m_current);
}

void PageHoverLink::set_send(Send send)
{
    MutexLocker locker(m_mutex);
    m_send = move(send);
}

}
