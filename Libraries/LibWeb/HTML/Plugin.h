/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin
class Plugin : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Plugin, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Plugin);

public:
    [[nodiscard]] static GC::Ref<Plugin> create(Window&, String name);

    virtual ~Plugin() override;

    Utf16FlyString const& name() const;
    Utf16FlyString description() const;
    Utf16FlyString filename() const;
    size_t length() const;
    GC::Ptr<MimeType> item(u32 index) const;
    GC::Ptr<MimeType> named_item(Utf16FlyString const& name) const;

private:
    Plugin(Window&, String name);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/system-state.html#concept-plugin-name
    Utf16FlyString m_name;

    GC::Ref<Window> m_window;

    // ^Bindings::Wrappable
    virtual Vector<Utf16FlyString> supported_property_names() const override;
};

}
