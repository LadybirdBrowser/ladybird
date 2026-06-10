/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#nodelist
class WEB_API NodeList : public Bindings::Wrappable {
    WEB_WRAPPABLE(NodeList, Bindings::Wrappable);

public:
    virtual ~NodeList() override;

    virtual u32 length() const = 0;
    virtual Node const* item(u32 index) const = 0;

protected:
    explicit NodeList();
};

}
