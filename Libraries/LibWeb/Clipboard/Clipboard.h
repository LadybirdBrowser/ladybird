/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct ClipboardUnsanitizedFormats;

}

namespace Web::Clipboard {

using ClipboardReadOptions = Bindings::ClipboardUnsanitizedFormats;

class Clipboard final : public DOM::EventTarget {
    WEB_WRAPPABLE(Clipboard, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Clipboard);

public:
    static GC::Ref<Clipboard> create();
    virtual ~Clipboard() override;

    void read(JS::Realm&, ClipboardReadOptions formats, GC::Ref<WebIDL::Promise>);
    void read_text(JS::Realm&, GC::Ref<WebIDL::Promise>);

    void write(JS::Realm&, GC::RootVector<GC::Ref<ClipboardItem>> const&, GC::Ref<WebIDL::Promise>);
    void write_text(JS::Realm&, String, GC::Ref<WebIDL::Promise>);

private:
    Clipboard();
};

}
