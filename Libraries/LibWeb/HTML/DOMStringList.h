/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class DOMStringList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMStringList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DOMStringList);

public:
    static JS::NonnullGCPtr<DOMStringList> create(JS::Realm&, Vector<String>);

    u32 length() const;
    Optional<String> item(u32 index) const;
    bool contains(StringView string);

    virtual Optional<JS::Value> item_value(size_t index) const override;

private:
    explicit DOMStringList(JS::Realm&, Vector<String>);

    virtual void initialize(JS::Realm&) override;

    Vector<String> m_list;
};

}
