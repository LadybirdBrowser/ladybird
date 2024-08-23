/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/EntriesAPI/FileSystemEntry.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitem-interface
class DataTransferItem : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DataTransferItem, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DataTransferItem);

public:
    static JS::NonnullGCPtr<DataTransferItem> create(JS::Realm&, JS::NonnullGCPtr<DataTransfer>, size_t item_index);
    virtual ~DataTransferItem() override;

    String kind() const;
    String type() const;

    void get_as_string(JS::GCPtr<WebIDL::CallbackType>) const;
    JS::GCPtr<FileAPI::File> get_as_file() const;

    JS::GCPtr<EntriesAPI::FileSystemEntry> webkit_get_as_entry() const;

private:
    DataTransferItem(JS::Realm&, JS::NonnullGCPtr<DataTransfer>, size_t item_index);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    Optional<DragDataStore::Mode> mode() const;

    JS::NonnullGCPtr<DataTransfer> m_data_transfer;
    Optional<size_t> m_item_index;
};

}
