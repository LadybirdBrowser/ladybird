/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitemlist-interface
class DataTransferItemList : public Bindings::Wrappable {
    WEB_WRAPPABLE(DataTransferItemList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DataTransferItemList);

public:
    static GC::Ref<DataTransferItemList> create(GC::Ref<DataTransfer>);
    virtual ~DataTransferItemList() override;

    WebIDL::UnsignedLong length() const;
    GC::Ptr<DataTransferItem> item(size_t index) const;

    WebIDL::ExceptionOr<GC::Ptr<DataTransferItem>> add(String const& data, String const& type);
    GC::Ptr<DataTransferItem> add(GC::Ref<FileAPI::File>);
    WebIDL::ExceptionOr<void> remove(WebIDL::UnsignedLong index);
    void clear();

private:
    explicit DataTransferItemList(GC::Ref<DataTransfer>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<DataTransfer> m_data_transfer;
};

}
