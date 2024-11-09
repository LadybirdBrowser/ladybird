/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/LiveNodeList.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#radionodelist
class RadioNodeList : public DOM::LiveNodeList {
    WEB_PLATFORM_OBJECT(RadioNodeList, DOM::LiveNodeList);
    JS_DECLARE_ALLOCATOR(RadioNodeList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<RadioNodeList> create(JS::Realm& realm, DOM::Node const& root, Scope scope, ESCAPING Function<bool(DOM::Node const&)> filter);

    virtual ~RadioNodeList() override;

    FlyString value() const;
    void set_value(FlyString const&);

protected:
    virtual void initialize(JS::Realm&) override;

private:
    explicit RadioNodeList(JS::Realm& realm, DOM::Node const& root, Scope scope, ESCAPING Function<bool(DOM::Node const&)> filter);
};

}
