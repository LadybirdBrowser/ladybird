/*
 * Copyright (c) 2020-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <LibJS/Heap/Handle.h>
#include <WebContent/Forward.h>

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

private:
    explicit PageHost(ConnectionFromClient&);

    ConnectionFromClient& m_client;
    HashMap<u64, JS::Handle<PageClient>> m_pages;
    u64 m_next_id { 0 };
};

}
