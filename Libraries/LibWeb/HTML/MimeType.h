/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#mimetype
class MimeType : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MimeType, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MimeType);

public:
    [[nodiscard]] static GC::Ref<MimeType> create(Window&, String type);

    virtual ~MimeType() override;

    Utf16FlyString const& type() const;
    Utf16FlyString description() const;
    Utf16FlyString suffixes() const;
    GC::Ref<Plugin> enabled_plugin() const;

private:
    MimeType(Window&, Utf16FlyString type);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/system-state.html#concept-mimetype-type
    Utf16FlyString m_type;

    GC::Ref<Window> m_window;
};

}
