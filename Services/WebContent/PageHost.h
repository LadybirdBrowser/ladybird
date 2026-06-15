/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <LibGC/Root.h>
#include <WebContent/Forward.h>

namespace Web {

namespace Compositor {

class CompositorHost;

}

}

namespace WebContent {

class PageHost {
    AK_MAKE_NONCOPYABLE(PageHost);
    AK_MAKE_NONMOVABLE(PageHost);

public:
    static NonnullOwnPtr<PageHost> create(ConnectionFromClient& client) { return adopt_own(*new PageHost(client)); }
    virtual ~PageHost();

    void initialize(u64 initial_page_id);
    Optional<PageClient&> page(u64 page_id);
    PageClient& create_page(u64 page_id);
    void remove_page(Badge<PageClient>, u64 page_id);

    ConnectionFromClient& client() const { return m_client; }
    void ensure_compositor_host();
    void compositor_process_reconnected();
    void compositor_process_lost();
    Web::Compositor::CompositorHost* compositor_host() { return m_compositor_host.ptr(); }
    Web::Compositor::CompositorHost const* compositor_host() const { return m_compositor_host.ptr(); }

private:
    explicit PageHost(ConnectionFromClient&);

    ConnectionFromClient& m_client;
    OwnPtr<Web::Compositor::CompositorHost> m_compositor_host;
    HashMap<u64, GC::Root<PageClient>> m_pages;
};

}
