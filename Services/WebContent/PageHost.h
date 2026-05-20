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

enum class DisplayListPlayerType;

namespace Compositor {

class CompositorThread;

}

}

namespace WebContent {

class PageHost {
    AK_MAKE_NONCOPYABLE(PageHost);
    AK_MAKE_NONMOVABLE(PageHost);

public:
    static NonnullOwnPtr<PageHost> create(ConnectionFromClient& client) { return adopt_own(*new PageHost(client)); }
    virtual ~PageHost();

    Optional<PageClient&> page(u64 index);
    PageClient& create_page();
    void remove_page(Badge<PageClient>, u64 index);

    ConnectionFromClient& client() const { return m_client; }
    void ensure_compositor_thread(Web::DisplayListPlayerType);
    Web::Compositor::CompositorThread* compositor_thread() { return m_compositor_thread.ptr(); }
    Web::Compositor::CompositorThread const* compositor_thread() const { return m_compositor_thread.ptr(); }

private:
    explicit PageHost(ConnectionFromClient&);

    ConnectionFromClient& m_client;
    OwnPtr<Web::Compositor::CompositorThread> m_compositor_thread;
    HashMap<u64, GC::Root<PageClient>> m_pages;
    u64 m_next_id { 0 };
};

}
