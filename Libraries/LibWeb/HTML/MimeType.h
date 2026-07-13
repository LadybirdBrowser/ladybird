/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#mimetype
class MimeType : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MimeType, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MimeType);

public:
    virtual ~MimeType() override;

    Utf16FlyString const& type() const;
    Utf16FlyString description() const;
    Utf16FlyString suffixes() const;
    GC::Ref<Plugin> enabled_plugin() const;

private:
    MimeType(JS::Realm&, Utf16FlyString type);

    virtual void initialize(JS::Realm&) override;

    // https://html.spec.whatwg.org/multipage/system-state.html#concept-mimetype-type
    Utf16FlyString m_type;
};

}
