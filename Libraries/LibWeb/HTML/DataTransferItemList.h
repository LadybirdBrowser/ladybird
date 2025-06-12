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
    GC_DECLARE_ALLOCATOR(DataTransferItemList);

public:
    static GC::Ref<DataTransferItemList> create(JS::Realm&, GC::Ref<DataTransfer>);
    virtual ~DataTransferItemList() override;

    WebIDL::UnsignedLong length() const;

    WebIDL::ExceptionOr<GC::Ptr<DataTransferItem>> add(String const& data, String const& type);
    GC::Ptr<DataTransferItem> add(GC::Ref<FileAPI::File>);

private:
    DataTransferItemList(JS::Realm&, GC::Ref<DataTransfer>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    virtual Optional<JS::Value> item_value(size_t index) const override;

    GC::Ref<DataTransfer> m_data_transfer;
};

}
