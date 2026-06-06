/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/DataTransferItem.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/EntriesAPI/FileSystemEntry.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitem-interface
class DataTransferItem : public Bindings::Wrappable {
    WEB_WRAPPABLE(DataTransferItem, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DataTransferItem);

public:
    static GC::Ref<DataTransferItem> create(GC::Ref<DataTransfer>, size_t item_index);
    virtual ~DataTransferItem() override;

    String kind() const;
    String type() const;
    void set_item_index(Badge<DataTransfer>, Optional<size_t> index) { m_item_index = move(index); }

    void get_as_string(JS::Realm&, GC::Ptr<WebIDL::CallbackType>) const;
    GC::Ptr<FileAPI::File> get_as_file() const;

    GC::Ptr<EntriesAPI::FileSystemEntry> webkit_get_as_entry() const;

private:
    DataTransferItem(GC::Ref<DataTransfer>, size_t item_index);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Optional<DragDataStore::Mode> mode() const;

    GC::Ref<DataTransfer> m_data_transfer;
    Optional<size_t> m_item_index;
};

}
