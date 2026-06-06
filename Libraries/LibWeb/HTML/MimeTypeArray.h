/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MimeTypeArray.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#mimetypearray
class MimeTypeArray : public Bindings::Wrappable {
    WEB_WRAPPABLE(MimeTypeArray, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MimeTypeArray);

public:
    [[nodiscard]] static GC::Ref<MimeTypeArray> create(Window&);

    virtual ~MimeTypeArray() override;

    size_t length() const;
    GC::Ptr<MimeType> item(u32 index) const;
    GC::Ptr<MimeType> named_item(FlyString const& name) const;

private:
    MimeTypeArray(Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<Window> m_window;

    // ^Bindings::Wrappable
    virtual Vector<FlyString> supported_property_names() const override;
    virtual Optional<JS::Value> item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const override;
    virtual JS::Value named_item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const override;
};

}
