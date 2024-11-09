/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitemlist-interface
class DataTransferItemList : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DataTransferItemList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DataTransferItemList);

public:
    static JS::NonnullGCPtr<DataTransferItemList> create(JS::Realm&, JS::NonnullGCPtr<DataTransfer>);
    virtual ~DataTransferItemList() override;

    WebIDL::UnsignedLong length() const;

    WebIDL::ExceptionOr<JS::GCPtr<DataTransferItem>> add(String const& data, String const& type);
    JS::GCPtr<DataTransferItem> add(JS::NonnullGCPtr<FileAPI::File>);

private:
    DataTransferItemList(JS::Realm&, JS::NonnullGCPtr<DataTransfer>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    virtual Optional<JS::Value> item_value(size_t index) const override;

    JS::NonnullGCPtr<DataTransfer> m_data_transfer;
};

}
