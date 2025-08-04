/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::CookieStore {

// https://cookiestore.spec.whatwg.org/#cookiestore
class CookieStore final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

private:
    CookieStore(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
