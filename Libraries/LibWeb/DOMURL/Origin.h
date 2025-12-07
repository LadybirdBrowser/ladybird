/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOMURL {

// https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-interface
class Origin : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Origin, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Origin);

public:
    static GC::Ref<Origin> construct_impl(JS::Realm&);
    static WebIDL::ExceptionOr<GC::Ref<Origin>> from(JS::VM&, JS::Value);

    bool opaque() const;
    bool is_same_origin(Origin const&) const;
    bool is_same_site(Origin const&) const;
    virtual Optional<URL::Origin> extract_an_origin() const override;

    virtual ~Origin() override;

private:
    Origin(JS::Realm&, URL::Origin);
    virtual void initialize(JS::Realm&) override;

    // https://html.spec.whatwg.org/multipage/browsers.html#concept-origin-origin
    // Origin objects have an associated origin, which holds an origin.
    URL::Origin m_origin;
};

}
