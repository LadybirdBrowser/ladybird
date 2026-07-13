/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/Find.h>
#include <AK/Utf16String.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransfer.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DataTransferItem.h>
#include <LibWeb/HTML/DataTransferItemList.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DataTransfer);

namespace DataTransferEffect {

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

struct NormalizedFormatForLookup {
    Utf16View type;
    bool convert_to_url { false };
};

static NormalizedFormatForLookup normalize_format_for_lookup(Utf16View format)
{
    if (format.equals_ignoring_ascii_case(u"text"sv))
        return { u"text/plain"sv };
    if (format.equals_ignoring_ascii_case(u"url"sv))
        return { u"text/uri-list"sv, true };
    return { format };
}

static Utf16FlyString normalize_format_for_storage(Utf16View format)
{
    if (format.equals_ignoring_ascii_case(u"text"sv))
        return "text/plain"_utf16_fly_string;
    if (format.equals_ignoring_ascii_case(u"url"sv))
        return "text/uri-list"_utf16_fly_string;
    return format.to_ascii_lowercase();
}

GC::Ref<DataTransfer> DataTransfer::create(JS::Realm& realm, NonnullRefPtr<DragDataStore> drag_data_store)
{
    return realm.create<DataTransfer>(realm, move(drag_data_store));
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer
GC::Ref<DataTransfer> DataTransfer::construct_impl(JS::Realm& realm)
{
    // 1. Set the drag data store's item list to be an empty list.
    auto drag_data_store = DragDataStore::create();

    // 2. Set the drag data store's mode to read/write mode.
    drag_data_store->set_mode(DragDataStore::Mode::ReadWrite);

    // 3. Set the dropEffect and effectAllowed to "none".
    // NOTE: This is done by the default-initializers.

    return realm.create<DataTransfer>(realm, move(drag_data_store));
}

DataTransfer::DataTransfer(JS::Realm& realm, NonnullRefPtr<DragDataStore> drag_data_store)
    : PlatformObject(realm)
    , m_associated_drag_data_store(move(drag_data_store))
{
    for (auto const& [i, item] : enumerate(m_associated_drag_data_store->item_list())) {
        auto data_transfer_item = DataTransferItem::create(realm, *this, i);
        m_item_list.append(data_transfer_item);
    }

    update_data_transfer_types_list();
}

DataTransfer::~DataTransfer() = default;

void DataTransfer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransfer);
    Base::initialize(realm);
}

void DataTransfer::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_items);
    visitor.visit(m_item_list);

    VISIT_CACHED_ATTRIBUTE(types);
}

void DataTransfer::set_drop_effect(Utf16FlyString drop_effect)
{
    using namespace DataTransferEffect;

    // On setting, if the new value is one of "none", "copy", "link", or "move", then the attribute's current value must
    // be set to the new value. Other values must be ignored.
    if (drop_effect.is_one_of(none, copy, link, move))
        m_drop_effect = AK::move(drop_effect);
}

void DataTransfer::set_effect_allowed(Utf16FlyString effect_allowed)
{
    // On setting, if drag data store's mode is the read/write mode and the new value is one of "none", "copy", "copyLink",
    // "copyMove", "link", "linkMove", "move", "all", or "uninitialized", then the attribute's current value must be set
    // to the new value. Otherwise, it must be left unchanged.
    if (m_associated_drag_data_store && m_associated_drag_data_store->mode() == DragDataStore::Mode::ReadWrite)
        set_effect_allowed_internal(move(effect_allowed));
}

void DataTransfer::set_effect_allowed_internal(Utf16FlyString effect_allowed)
{
    // AD-HOC: We need to be able to set the effectAllowed attribute internally regardless of the state of the drag data store.
    using namespace DataTransferEffect;

    if (effect_allowed.is_one_of(none, copy, copyLink, copyMove, link, linkMove, move, all, uninitialized))
        m_effect_allowed = AK::move(effect_allowed);
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-items
GC::Ref<DataTransferItemList> DataTransfer::items()
{
    // The items attribute must return a DataTransferItemList object associated with the DataTransfer object.
    if (!m_items)
        m_items = DataTransferItemList::create(realm(), *this);
    return *m_items;
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-types
ReadonlySpan<Utf16String> DataTransfer::types() const
{
    // The types attribute must return this DataTransfer object's types array.
    return m_types;
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-getdata
Utf16String DataTransfer::get_data(Utf16View format_argument) const
{
    // 1. If the DataTransfer object is no longer associated with a drag data store, then return the empty string.
    if (!m_associated_drag_data_store)
        return {};

    // 2. If the drag data store's mode is the protected mode, then return the empty string.
    if (m_associated_drag_data_store->mode() == DragDataStore::Mode::Protected)
        return {};

    // 3. Let format be the first argument, converted to ASCII lowercase.
    // 4. Let convert-to-URL be false.
    // 5. If format equals "text", change it to "text/plain".
    // 6. If format equals "url", change it to "text/uri-list" and set convert-to-URL to true.
    auto format = normalize_format_for_lookup(format_argument);
    [[maybe_unused]] auto convert_to_url = format.convert_to_url;

    // 7. If there is no item in the drag data store item list whose kind is text and whose type string is equal to
    //    format, return the empty string.
    auto item_list = m_associated_drag_data_store->item_list();

    auto it = find_if(item_list.begin(), item_list.end(), [&](auto const& item) {
        return item.kind == DragDataStoreItem::Kind::Text && item.type_string.equals_ignoring_ascii_case(format.type);
    });

    if (it == item_list.end())
        return {};

    // 8. Let result be the data of the item in the drag data store item list whose kind is Plain Unicode string and
    //    whose type string is equal to format.
    auto const& result = it->data;

    // FIXME: 9. If convert-to-URL is true, then parse result as appropriate for text/uri-list data, and then set result to
    //           the first URL from the list, if any, or the empty string otherwise.

    // 10. Return result.
    return result;
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-setdata
void DataTransfer::set_data(Utf16View format_argument, Utf16View value)
{
    // 1. If the DataTransfer object is no longer associated with a drag data store, then return.
    if (!m_associated_drag_data_store)
        return;

    // 2. If the drag data store's mode is not the read/write mode, return. Nothing happens.
    if (m_associated_drag_data_store->mode() != DragDataStore::Mode::ReadWrite)
        return;

    // 3. Let format be the first argument, converted to ASCII lowercase.
    auto format = normalize_format_for_storage(format_argument);

    // 5. Remove the item in the drag data store item list whose kind is text and whose type string is equal to format, if there is one.
    for (auto const& [i, item] : enumerate(m_associated_drag_data_store->item_list())) {
        if (item.kind == DragDataStoreItem::Kind::Text && item.type_string == format) {
            remove_item(i);
            break;
        }
    }

    // 6. Add an item to the drag data store item list
    // whose kind is text, whose type string is equal to format, and whose data is the string given by the method's second argument.
    DragDataStoreItem item {};
    item.kind = DragDataStoreItem::Kind::Text;
    item.type_string = format;
    item.data = Utf16String::from_utf16(value);
    add_item(move(item));

    update_data_transfer_types_list();
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-files
GC::Ref<FileAPI::FileList> DataTransfer::files() const
{
    auto& realm = this->realm();

    // 1. Start with an empty list L.
    auto files = FileAPI::FileList::create(realm);

    // 2. If the DataTransfer object is no longer associated with a drag data store, the FileList is empty. Return
    //    the empty list L.
    if (!m_associated_drag_data_store)
        return files;

    // 3. If the drag data store's mode is the protected mode, return the empty list L.
    if (m_associated_drag_data_store->mode() == DragDataStore::Mode::Protected)
        return files;

    // 4. For each item in the drag data store item list whose kind is File, add the item's data (the file, in
    //    particular its name and contents, as well as its type) to the list L.
    for (auto const& item : m_associated_drag_data_store->item_list()) {
        if (item.kind != DragDataStoreItem::Kind::File)
            continue;

        auto blob = FileAPI::Blob::create(realm, item.file_data, item.type_string.view());

        // FIXME: Fill in other fields (e.g. last_modified).
        Bindings::FilePropertyBag options {};
        options.type = item.type_string.to_utf16_string();

        auto file = MUST(FileAPI::File::create(realm, { { blob } }, item.file_name, move(options)));
        files->add_file(file);
    }

    // 5. The files found by these steps are those in the list L.
    return files;
}

Optional<DragDataStore::Mode> DataTransfer::mode() const
{
    if (m_associated_drag_data_store)
        return m_associated_drag_data_store->mode();
    return {};
}

void DataTransfer::disassociate_with_drag_data_store()
{
    m_associated_drag_data_store.clear();
    update_data_transfer_types_list();
}

GC::Ref<DataTransferItem> DataTransfer::add_item(DragDataStoreItem item)
{
    auto& realm = this->realm();

    VERIFY(m_associated_drag_data_store);
    m_associated_drag_data_store->add_item(move(item));

    auto data_transfer_item = DataTransferItem::create(realm, *this, m_associated_drag_data_store->size() - 1);
    m_item_list.append(data_transfer_item);

    update_data_transfer_types_list();

    return data_transfer_item;
}

void DataTransfer::remove_item(size_t index)
{
    VERIFY(m_associated_drag_data_store);
    VERIFY(index < m_item_list.size());

    m_associated_drag_data_store->remove_item_at(index);
    auto& item = m_item_list.at(index);
    item->set_item_index({}, OptionalNone {});
    m_item_list.remove(index);
    for (size_t i = index; i < m_item_list.size(); ++i) {
        m_item_list.at(i)->set_item_index({}, i);
    }

    update_data_transfer_types_list();
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-cleardata
void DataTransfer::clear_data(Optional<Utf16String> const& maybe_format)
{
    // 1. If the DataTransfer object is no longer associated with a drag data store, return. Nothing happens.
    if (!m_associated_drag_data_store)
        return;

    // 2. If the drag data store's mode is not the read/write mode, return. Nothing happens.
    if (m_associated_drag_data_store->mode() != DragDataStore::Mode::ReadWrite)
        return;

    auto remove_items_from_drag_data_store = [&](Optional<Utf16View> format = {}) {
        for (size_t i = m_associated_drag_data_store->item_list().size(); i > 0; --i) {
            auto const& item = m_associated_drag_data_store->item_list().at(i - 1);
            if (item.kind == DragDataStoreItem::Kind::Text && (!format.has_value() || item.type_string.equals_ignoring_ascii_case(*format)))
                remove_item(i - 1);
        }
    };

    // 3. If the method was called with no arguments, remove each item in the drag data store item list whose kind is
    //    Plain Unicode string, and return.
    if (!maybe_format.has_value()) {
        remove_items_from_drag_data_store();
        return;
    }

    // 4. Set format to format, converted to ASCII lowercase.
    auto format = normalize_format_for_lookup(maybe_format->utf16_view());

    // 6. Remove each item in the drag data store item list whose kind is Plain Unicode string and whose type string is equal to format.
    remove_items_from_drag_data_store(format.type);
}

bool DataTransfer::contains_item_with_type(DragDataStoreItem::Kind kind, Utf16View type) const
{
    VERIFY(m_associated_drag_data_store);

    for (auto const& item : m_associated_drag_data_store->item_list()) {
        if (item.kind == kind && item.type_string.equals_ignoring_ascii_case(type))
            return true;
    }

    return false;
}

GC::Ref<DataTransferItem> DataTransfer::item(size_t index) const
{
    VERIFY(index < m_item_list.size());
    return m_item_list[index];
}

DragDataStoreItem const& DataTransfer::drag_data(size_t index) const
{
    VERIFY(m_associated_drag_data_store);
    VERIFY(index < m_item_list.size());

    return m_associated_drag_data_store->item_list()[index];
}

size_t DataTransfer::length() const
{
    if (m_associated_drag_data_store)
        return m_associated_drag_data_store->size();
    return 0;
}

// https://html.spec.whatwg.org/multipage/dnd.html#concept-datatransfer-types
void DataTransfer::update_data_transfer_types_list()
{
    // 1. Let L be an empty sequence.
    Vector<Utf16String> types;

    // 2. If the DataTransfer object is still associated with a drag data store, then:
    if (m_associated_drag_data_store) {
        bool contains_file = false;

        // 1. For each item in the DataTransfer object's drag data store item list whose kind is text, add an entry to L
        //    consisting of the item's type string.
        for (auto const& item : m_associated_drag_data_store->item_list()) {
            switch (item.kind) {
            case DragDataStoreItem::Kind::Text:
                types.append(item.type_string.to_utf16_string());
                break;
            case DragDataStoreItem::Kind::File:
                contains_file = true;
                break;
            }
        }

        // 2. If there are any items in the DataTransfer object's drag data store item list whose kind is File, then add
        //    an entry to L consisting of the string "Files". (This value can be distinguished from the other values
        //    because it is not lowercase.)
        if (contains_file)
            types.append("Files"_utf16);
    }

    // 3. Set the DataTransfer object's types array to the result of creating a frozen array from L.
    set_cached_types(nullptr);
    m_types = move(types);
}

}
