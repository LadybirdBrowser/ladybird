/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitem-interface
class DataTransferItem : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DataTransferItem, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DataTransferItem);

public:
    virtual ~DataTransferItem() override;

private:
    DataTransferItem(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
