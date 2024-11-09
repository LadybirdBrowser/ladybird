/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#pluginarray
class PluginArray : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PluginArray, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(PluginArray);

public:
    virtual ~PluginArray() override;

    void refresh() const;
    size_t length() const;
    JS::GCPtr<Plugin> item(u32 index) const;
    JS::GCPtr<Plugin> named_item(FlyString const& name) const;

private:
    PluginArray(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    // ^Bindings::PlatformObject
    virtual Vector<FlyString> supported_property_names() const override;
    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual JS::Value named_item_value(FlyString const& name) const override;
};

}
