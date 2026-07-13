/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class DOMStringList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMStringList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMStringList);

public:
    static GC::Ref<DOMStringList> create(JS::Realm&, Vector<Utf16String>);

    u32 length() const;
    Optional<Utf16String> item(u32 index) const;
    bool contains(Utf16View string);

    virtual Optional<JS::Value> item_value(size_t index) const override;

private:
    explicit DOMStringList(JS::Realm&, Vector<Utf16String>);

    virtual void initialize(JS::Realm&) override;

    Vector<Utf16String> m_list;
};

}
