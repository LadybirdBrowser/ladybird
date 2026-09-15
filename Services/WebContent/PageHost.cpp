/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>

#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>
#include <WebContent/WebContentCompositorHost.h>
#include <WebContent/WebDriverConnection.h>

namespace WebContent {

PageHost::PageHost(ConnectionFromClient& client)
    : m_client(client)
{
}

void PageHost::initialize(Web::PageId initial_page_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::CrossProcessIdAllocator cross_process_id_allocator, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::VisibilityState system_visibility_state)
{
    VERIFY(m_pages.is_empty());
    m_cross_process_id_allocator = cross_process_id_allocator;

    // The process displays a tab, whose traversable takes the id the UI process gave it, or hosts an isolated iframe
    // of a tab whose graph it is given whole.
    if (remote_navigables.is_empty()) {
        auto& first_page = create_page(initial_page_id, root_navigable_id);
        Web::HTML::LocalTraversableNavigable::create_a_fresh_top_level_traversable(first_page.page(), URL::about_blank(), Empty {}, move(initial_history_entry), system_visibility_state);
        return;
    }
    auto& first_page = create_page(initial_page_id);
    first_page.page().create_remote_navigable_graph(move(remote_navigables));
    first_page.page().begin_hosting(root_navigable_id, initial_history_entry, system_visibility_state);
}

PageClient& PageHost::create_page(Web::PageId page_id, Optional<Web::HTML::CrossProcessId> pending_root_navigable_id)
{
    VERIFY(page_id > 0);
    VERIFY(!m_pages.contains(page_id));
    m_pages.set(page_id, PageClient::create(*this, page_id, pending_root_navigable_id));
    return *m_pages.get(page_id).value();
}

Web::HTML::CrossProcessId PageHost::allocate_cross_process_id()
{
    VERIFY(m_cross_process_id_allocator.has_value());
    return m_cross_process_id_allocator->allocate();
}

Web::HTML::CrossProcessId PageHost::allocate_navigable_id()
{
    return allocate_cross_process_id();
}

void PageHost::remove_page(Badge<PageClient>, Web::PageId page_id)
{
    m_pages.remove(page_id);
}

Optional<PageClient&> PageHost::page(Web::PageId page_id)
{
    return m_pages.get(page_id).map([](auto& value) -> PageClient& {
        return *value;
    });
}

PageHost::~PageHost() = default;

void PageHost::ensure_compositor_host()
{
    if (m_compositor_host)
        return;
    m_compositor_host = create_web_content_compositor_host(m_client);
}

void PageHost::compositor_process_reconnected()
{
    for (auto& [_, page] : m_pages)
        page->compositor_process_reconnected();
}

void PageHost::invalidate_user_style()
{
    for (auto& [_, page] : m_pages)
        page->page().invalidate_user_style();
}

void PageHost::compositor_process_lost()
{
    for (auto& [_, page] : m_pages)
        page->compositor_process_lost();
}

}
