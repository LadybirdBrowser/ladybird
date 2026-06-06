/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Plugin.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin
class Plugin : public Bindings::Wrappable {
    WEB_WRAPPABLE(Plugin, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Plugin);

public:
    virtual ~Plugin() override;

    String const& name() const;
    String description() const;
    String filename() const;
    size_t length() const;
    GC::Ptr<MimeType> item(u32 index) const;
    GC::Ptr<MimeType> named_item(FlyString const& name) const;

private:
    Plugin(JS::Realm&, String name);

    // https://html.spec.whatwg.org/multipage/system-state.html#concept-plugin-name
    String m_name;

    // ^Bindings::Wrappable
    virtual Vector<FlyString> supported_property_names() const override;
    virtual Optional<JS::Value> item_value(JS::Realm& realm, size_t index) const override;
    virtual JS::Value named_item_value(JS::Realm& realm, FlyString const& name) const override;
};

}
