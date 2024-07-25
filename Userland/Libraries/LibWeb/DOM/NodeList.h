/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#nodelist
class NodeList : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NodeList, Bindings::PlatformObject);

public:
    virtual ~NodeList() override;

    virtual u32 length() const = 0;
    virtual Node const* item(u32 index) const = 0;

    virtual Optional<JS::Value> item_value(size_t index) const override;

protected:
    explicit NodeList(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
