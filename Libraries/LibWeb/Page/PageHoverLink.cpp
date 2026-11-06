/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/Page/PageHoverLink.h>

namespace Web {

// NB: A blob URL's entry is counted without atomics, and a client only shows the URL, so none crosses threads here.
static void forget_blob_url_entry(Optional<URL::URL>& url)
{
    if (url.has_value())
        url->set_blob_url_entry({});
}

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

void PageHoverLink::update(Optional<URL::URL> url)
{
    if (m_current == url)
        return;
    m_current = move(url);
    if (m_send)
        m_send(m_current);
}

void PageHoverLink::request(Optional<URL::URL> url, Optional<UniqueNodeID> document)
{
    forget_blob_url_entry(url);
    MutexLocker locker(m_mutex);
    m_document = document;
    update(move(url));
}

void PageHoverLink::withdraw(UniqueNodeID document)
{
    MutexLocker locker(m_mutex);
    if (m_document != document)
        return;
    m_document = {};
    update({});
}

void PageHoverLink::set_send(Send send)
{
    MutexLocker locker(m_mutex);
    m_send = move(send);
}

NonnullRefPtr<HoverLinkReporter> HoverLinkReporter::create(NonnullRefPtr<PageHoverLink> hover_link, UniqueNodeID document)
{
    return adopt_ref(*new HoverLinkReporter(move(hover_link), document));
}

HoverLinkReporter::HoverLinkReporter(NonnullRefPtr<PageHoverLink> hover_link, UniqueNodeID document)
    : m_hover_link(move(hover_link))
    , m_document(document)
{
}

bool HoverLinkReporter::resolves_as(String const& serialized_base_url, Utf16String const& encoding) const
{
    MutexLocker locker(m_mutex);
    return m_base_url.serialize() == serialized_base_url && m_encoding == encoding;
}

void HoverLinkReporter::set_base(URL::URL base_url, Utf16String encoding)
{
    base_url.set_blob_url_entry({});
    auto encoding_name = encoding.to_utf8();
    MutexLocker locker(m_mutex);
    m_base_url = move(base_url);
    m_encoding = move(encoding);
    m_encoding_name = move(encoding_name);
}

void HoverLinkReporter::set_link(u32 element, Optional<Utf16String> href)
{
    MutexLocker locker(m_mutex);
    if (href.has_value())
        m_links.set(element, href.release_value());
    else
        m_links.remove(element);
}

bool HoverLinkReporter::is_link(u32 element) const
{
    MutexLocker locker(m_mutex);
    return m_links.contains(element);
}

void HoverLinkReporter::report(u32 element)
{
    // NB: The lock keeps the string live here, so this thread takes no reference to it. And it keeps a report from
    //     coming after retire(), and so after the hover of a replaced document has ended.
    MutexLocker locker(m_mutex);
    if (m_is_retired)
        return;
    Optional<URL::URL> url;
    if (auto link = m_links.find(element); link != m_links.end())
        url = URL::Parser::basic_parse(link->value.utf16_view(), m_base_url, m_encoding_name.bytes_as_string_view());
    m_hover_link->request(move(url), m_document);
}

void HoverLinkReporter::withdraw()
{
    m_hover_link->withdraw(m_document);
}

void HoverLinkReporter::retire()
{
    MutexLocker locker(m_mutex);
    m_is_retired = true;
}

}
