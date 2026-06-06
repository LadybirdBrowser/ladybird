/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Clipboard.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Clipboard {

class Clipboard final : public DOM::EventTarget {
    WEB_WRAPPABLE(Clipboard, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Clipboard);

public:
    static GC::Ref<Clipboard> create();
    virtual ~Clipboard() override;

    GC::Ref<WebIDL::Promise> read(JS::Realm&, Bindings::ClipboardUnsanitizedFormats formats = {});
    GC::Ref<WebIDL::Promise> read_text(JS::Realm&);

    GC::Ref<WebIDL::Promise> write(JS::Realm&, GC::RootVector<GC::Ref<ClipboardItem>> const&);
    GC::Ref<WebIDL::Promise> write_text(JS::Realm&, String);

private:
    Clipboard();
};

}
