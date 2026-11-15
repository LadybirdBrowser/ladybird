/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibHTTP/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CookieStore.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::CookieStore {

class WEB_API CookieStore : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CookieStore, cookie_store)

public:
    static GC::Ref<CookieStore> create(JS::Realm&);

    void process_cookie_changes(Vector<HTTP::Cookie::Cookie> const&);
};

}
