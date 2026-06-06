/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PluginArray.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#pluginarray
class PluginArray : public Bindings::Wrappable {
    WEB_WRAPPABLE(PluginArray, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(PluginArray);

public:
    virtual ~PluginArray() override;

    void refresh() const;
    size_t length() const;
    GC::Ptr<Plugin> item(u32 index) const;
    GC::Ptr<Plugin> named_item(FlyString const& name) const;

private:
    PluginArray(JS::Realm&);

    // ^Bindings::Wrappable
    virtual Vector<FlyString> supported_property_names() const override;
    virtual Optional<JS::Value> item_value(JS::Realm& realm, size_t index) const override;
    virtual JS::Value named_item_value(JS::Realm& realm, FlyString const& name) const override;
};

}
