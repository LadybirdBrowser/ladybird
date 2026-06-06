/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MimeType.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#mimetype
class MimeType : public Bindings::Wrappable {
    WEB_WRAPPABLE(MimeType, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MimeType);

public:
    virtual ~MimeType() override;

    String const& type() const;
    String description() const;
    String const& suffixes() const;
    GC::Ref<Plugin> enabled_plugin() const;

private:
    MimeType(JS::Realm&, String type);

    // https://html.spec.whatwg.org/multipage/system-state.html#concept-mimetype-type
    String m_type;
};

}
