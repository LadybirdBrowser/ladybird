/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin
class Plugin : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Plugin, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Plugin);

public:
    virtual ~Plugin() override;

    Utf16FlyString const& name() const;
    Utf16FlyString description() const;
    Utf16FlyString filename() const;
    size_t length() const;
    GC::Ptr<MimeType> item(u32 index) const;
    GC::Ptr<MimeType> named_item(Utf16FlyString const& name) const;

private:
    Plugin(JS::Realm&, Utf16FlyString name);

    // https://html.spec.whatwg.org/multipage/system-state.html#concept-plugin-name
    Utf16FlyString m_name;

    virtual void initialize(JS::Realm&) override;

    // ^Bindings::PlatformObject
    virtual Vector<Utf16FlyString> supported_property_names() const override;
    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual JS::Value named_item_value(Utf16FlyString const& name) const override;
};

}
