/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

// https://wicg.github.io/cookie-store/#CookieStore
class CookieStore final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

public:
    [[nodiscard]] static GC::Ref<CookieStore> create(JS::Realm&, GC::Ref<Page>);
    virtual ~CookieStore() override = default;

private:
    CookieStore(JS::Realm&, GC::Ref<Page>);

    virtual void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

    GC::Ref<Page> m_page;
};

}
