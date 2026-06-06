/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RadioNodeList.h>
#include <LibWeb/DOM/LiveNodeList.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#radionodelist
class RadioNodeList : public DOM::LiveNodeList {
    WEB_WRAPPABLE(RadioNodeList, DOM::LiveNodeList);
    GC_DECLARE_ALLOCATOR(RadioNodeList);

public:
    [[nodiscard]] static GC::Ref<RadioNodeList> create(DOM::Node const& root, Scope scope, ESCAPING Function<bool(DOM::Node const&)> filter);

    virtual ~RadioNodeList() override;

    FlyString value() const;
    void set_value(FlyString const&);

private:
    explicit RadioNodeList(DOM::Node const& root, Scope scope, ESCAPING Function<bool(DOM::Node const&)> filter);
};

}
