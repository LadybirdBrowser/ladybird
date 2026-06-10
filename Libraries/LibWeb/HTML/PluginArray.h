/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#pluginarray
class PluginArray : public Bindings::Wrappable {
    WEB_WRAPPABLE(PluginArray, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(PluginArray);

public:
    [[nodiscard]] static GC::Ref<PluginArray> create(Window&);

    virtual ~PluginArray() override;

    void refresh() const;
    size_t length() const;
    GC::Ptr<Plugin> item(u32 index) const;
    GC::Ptr<Plugin> named_item(FlyString const& name) const;

private:
    PluginArray(Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<Window> m_window;

    // ^Bindings::Wrappable
    virtual Vector<FlyString> supported_property_names() const override;
};

}
