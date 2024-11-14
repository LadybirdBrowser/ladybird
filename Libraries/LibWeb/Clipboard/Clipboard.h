/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Clipboard {

class Clipboard final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Clipboard, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Clipboard);

public:
    static WebIDL::ExceptionOr<GC::Ref<Clipboard>> construct_impl(JS::Realm&);
    virtual ~Clipboard() override;

    GC::Ref<WebIDL::Promise> write_text(String);

private:
    Clipboard(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
