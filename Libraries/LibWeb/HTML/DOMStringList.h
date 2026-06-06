/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/DOMStringList.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

class DOMStringList final : public Bindings::Wrappable {
    WEB_WRAPPABLE(DOMStringList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMStringList);

public:
    static GC::Ref<DOMStringList> create(JS::Realm&, Vector<String>);

    u32 length() const;
    Optional<String> item(u32 index) const;
    bool contains(StringView string);

    virtual Optional<JS::Value> item_value(JS::Realm& realm, size_t index) const override;

private:
    explicit DOMStringList(JS::Realm&, Vector<String>);

    Vector<String> m_list;
};

}
