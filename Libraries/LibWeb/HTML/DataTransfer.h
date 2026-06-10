/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

#define ENUMERATE_DATA_TRANSFER_EFFECTS        \
    __ENUMERATE_DATA_TRANSFER_EFFECT(none)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(copy)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(copyLink) \
    __ENUMERATE_DATA_TRANSFER_EFFECT(copyMove) \
    __ENUMERATE_DATA_TRANSFER_EFFECT(link)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(linkMove) \
    __ENUMERATE_DATA_TRANSFER_EFFECT(move)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(all)      \
    __ENUMERATE_DATA_TRANSFER_EFFECT(uninitialized)

namespace DataTransferEffect {

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) extern FlyString const& name;
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransfer-interface
class DataTransfer : public Bindings::Wrappable {
    WEB_WRAPPABLE(DataTransfer, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DataTransfer);

public:
    static GC::Ref<DataTransfer> create(NonnullRefPtr<DragDataStore>);
    static GC::Ref<DataTransfer> create_for_constructor();
    virtual ~DataTransfer() override;

    FlyString const& drop_effect() const { return m_drop_effect; }
    void set_drop_effect(String const&);
    void set_drop_effect(FlyString);

    FlyString const& effect_allowed() const { return m_effect_allowed; }
    void set_effect_allowed(String const&);
    void set_effect_allowed(FlyString);
    void set_effect_allowed_internal(FlyString);

    GC::Ref<DataTransferItemList> items();

    ReadonlySpan<String> types_list() const;
    JS::ThrowCompletionOr<JS::Value> types(JS::Realm&);
    u64 types_revision() const { return m_types_revision; }
    String get_data(String const& format) const;
    void set_data(String const& format_argument, String const& value);
    void clear_data(Optional<String> maybe_format = {});
    GC::Ref<FileAPI::FileList> files() const;

    Optional<DragDataStore::Mode> mode() const;
    void disassociate_with_drag_data_store();

    GC::Ref<DataTransferItem> add_item(DragDataStoreItem item);
    void remove_item(size_t index);
    bool contains_item_with_type(DragDataStoreItem::Kind, String const& type) const;
    GC::Ref<DataTransferItem> item(size_t index) const;
    DragDataStoreItem const& drag_data(size_t index) const;
    size_t length() const;

private:
    explicit DataTransfer(NonnullRefPtr<DragDataStore>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    void update_data_transfer_types_list();

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-dropeffect
    FlyString m_drop_effect { DataTransferEffect::none };

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-effectallowed
    FlyString m_effect_allowed { DataTransferEffect::none };

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-items
    GC::Ptr<DataTransferItemList> m_items;
    Vector<GC::Ref<DataTransferItem>> m_item_list;

    // https://html.spec.whatwg.org/multipage/dnd.html#concept-datatransfer-types
    Vector<String> m_types;
    u64 m_types_revision { 0 };

    // https://html.spec.whatwg.org/multipage/dnd.html#the-datatransfer-interface:drag-data-store-3
    RefPtr<DragDataStore> m_associated_drag_data_store;
};

}
