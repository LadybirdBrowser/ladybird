/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/DragDataStore.h>
#include <LibWeb/WebIDL/CachedAttribute.h>

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

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) extern Utf16FlyString const& name;
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

// https://html.spec.whatwg.org/multipage/dnd.html#the-datatransfer-interface
class DataTransfer : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DataTransfer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DataTransfer);

public:
    static GC::Ref<DataTransfer> create(JS::Realm&, NonnullRefPtr<DragDataStore>);
    static GC::Ref<DataTransfer> construct_impl(JS::Realm&);
    virtual ~DataTransfer() override;

    Utf16FlyString const& drop_effect() const { return m_drop_effect; }
    void set_drop_effect(Utf16FlyString);

    Utf16FlyString const& effect_allowed() const { return m_effect_allowed; }
    void set_effect_allowed(Utf16FlyString);
    void set_effect_allowed_internal(Utf16FlyString);

    GC::Ref<DataTransferItemList> items();

    ReadonlySpan<Utf16String> types() const;
    DEFINE_CACHED_ATTRIBUTE(types);
    Utf16String get_data(Utf16View format) const;
    void set_data(Utf16View format_argument, Utf16View value);
    void clear_data(Optional<Utf16String> const& maybe_format = {});
    GC::Ref<FileAPI::FileList> files() const;

    Optional<DragDataStore::Mode> mode() const;
    void disassociate_with_drag_data_store();

    GC::Ref<DataTransferItem> add_item(DragDataStoreItem item);
    void remove_item(size_t index);
    bool contains_item_with_type(DragDataStoreItem::Kind, Utf16View type) const;
    GC::Ref<DataTransferItem> item(size_t index) const;
    DragDataStoreItem const& drag_data(size_t index) const;
    size_t length() const;

private:
    DataTransfer(JS::Realm&, NonnullRefPtr<DragDataStore>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    void update_data_transfer_types_list();

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-dropeffect
    Utf16FlyString m_drop_effect { DataTransferEffect::none };

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-effectallowed
    Utf16FlyString m_effect_allowed { DataTransferEffect::none };

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-items
    GC::Ptr<DataTransferItemList> m_items;
    Vector<GC::Ref<DataTransferItem>> m_item_list;

    // https://html.spec.whatwg.org/multipage/dnd.html#concept-datatransfer-types
    Vector<Utf16String> m_types;

    // https://html.spec.whatwg.org/multipage/dnd.html#the-datatransfer-interface:drag-data-store-3
    RefPtr<DragDataStore> m_associated_drag_data_store;
};

}
