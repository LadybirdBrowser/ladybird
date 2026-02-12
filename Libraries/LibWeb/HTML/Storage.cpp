/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StoragePrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/StorageEvent.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Storage);

static HashTable<GC::RawRef<Storage>>& all_storages()
{
    // FIXME: This needs to be stored at the user agent level.
    static HashTable<GC::RawRef<Storage>> storages;
    return storages;
}

GC::Ref<Storage> Storage::create(JS::Realm& realm, Type type, GC::Ref<StorageAPI::StorageBottle> storage_bottle)
{
    return realm.create<Storage>(realm, type, move(storage_bottle));
}

Storage::Storage(JS::Realm& realm, Type type, GC::Ref<StorageAPI::StorageBottle> storage_bottle)
    : Bindings::PlatformObject(realm)
    , m_type(type)
    , m_storage_bottle(move(storage_bottle))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = false,
        .supports_named_properties = true,
        .has_indexed_property_setter = true,
        .has_named_property_setter = true,
        .has_named_property_deleter = true,
        .indexed_property_setter_has_identifier = true,
        .named_property_setter_has_identifier = true,
        .named_property_deleter_has_identifier = true,
    };

    all_storages().set(*this);
}

Storage::~Storage() = default;

void Storage::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Storage);
    Base::initialize(realm);
}

void Storage::finalize()
{
    Base::finalize();
    all_storages().remove(*this);
}

void Storage::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_storage_bottle);
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-length
size_t Storage::length() const
{
    // The length getter steps are to return this's map's size.
    return m_storage_bottle->size();
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-key
Optional<String> Storage::key(size_t index)
{
    // 1. If index is greater than or equal to this's map's size, then return null.
    if (index >= m_storage_bottle->size())
        return {};

    // 2. Let keys be the result of running get the keys on this's map.
    auto keys = m_storage_bottle->keys();

    // 3. Return keys[index].
    return keys[index];
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-getitem
Optional<String> Storage::get_item(String const& key) const
{
    // 1. If this's map[key] does not exist, then return null.
    // 2. Return this's map[key].
    return m_storage_bottle->get(key);
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-setitem
WebIDL::ExceptionOr<void> Storage::set_item(String const& key, String const& value)
{
    // 1. Let oldValue be null.
    // 2. Let reorder be true.
    bool reorder = true;

    // 3. If this's map[key] exists:
    //     1. Set oldValue to this's map[key].
    //     2. If oldValue is value, then return.
    //     3. Set reorder to false.
    // 4. If value cannot be stored, then throw a "QuotaExceededError" DOMException.
    // 5. Set this's map[key] to value.

    auto result = m_storage_bottle->set(key, value);

    if (result.has<WebView::StorageOperationError>())
        return WebIDL::QuotaExceededError::create(realm(), Utf16String::formatted("Unable to store more than {} bytes in storage", *m_storage_bottle->quota()));

    auto old_value = result.get<Optional<String>>();

    if (old_value.has_value()) {
        if (old_value.value() == value)
            return {};

        reorder = false;
    }

    // 6. If reorder is true, then reorder this.
    if (reorder)
        this->reorder();

    // 7. Broadcast this with key, oldValue, and value.
    broadcast(key, old_value, value);

    return {};
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-removeitem
void Storage::remove_item(String const& key)
{
    // 1. If this's map[key] does not exist, then return.
    // 2. Set oldValue to this's map[key].
    auto old_value = m_storage_bottle->get(key);
    if (!old_value.has_value())
        return;

    // 3. Remove this's map[key].
    m_storage_bottle->remove(key);

    // 4. Reorder this.
    reorder();

    // 5. Broadcast this with key, oldValue, and null.
    broadcast(key, old_value, {});
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-clear
void Storage::clear()
{
    // 1. If this's map is empty, then return.
    if (m_storage_bottle->size() == 0)
        return;

    // 2. Clear this's map.
    m_storage_bottle->clear();

    // 3. Broadcast this with null, null, and null.
    broadcast({}, {}, {});
}

// https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-reorder
void Storage::reorder()
{
    // To reorder a Storage object storage, reorder storage's map's entries in an implementation-defined manner.
    // NOTE: This basically means that we're not required to maintain any particular iteration order.
}

// https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-broadcast
void Storage::broadcast(Optional<String> const& key, Optional<String> const& old_value, Optional<String> const& new_value)
{
    auto& realm = this->realm();

    // 1. Let thisDocument be storage's relevant global object's associated Document.
    auto& relevant_global = relevant_global_object(*this);
    auto const& this_document = as<Window>(relevant_global).associated_document();

    // 2. Let url be the serialization of thisDocument's URL.
    auto url = this_document.url().serialize();

    // 3. Let remoteStorages be all Storage objects excluding storage whose:
    GC::RootVector<GC::Ref<Storage>> remote_storages(heap());
    for (auto storage : all_storages()) {
        if (storage == this)
            continue;

        // * type is storage's type
        if (storage->type() != type())
            continue;

        // * relevant settings object's origin is same origin with storage's relevant settings object's origin
        if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(storage).origin()))
            continue;

        // * and, if type is "session", whose relevant settings object's associated Document's node navigable's traversable navigable
        //   is thisDocument's node navigable's traversable navigable.
        if (type() == Type::Session) {
            auto& storage_document = *relevant_settings_object(storage).responsible_document();

            // NB: It is possible the remote storage may have not been fully teared down immediately at the point it's
            //     document is made inactive.
            if (!storage_document.navigable())
                continue;

            // NB: It is possible for this storage's document to have lost its navigable if script holds a reference to
            //     the Storage object after its browsing context has navigated to a new document.
            if (!this_document.navigable())
                continue;

            if (storage_document.navigable()->traversable_navigable() != this_document.navigable()->traversable_navigable())
                continue;
        }

        remote_storages.append(storage);
    }

    // 4. For each remoteStorage of remoteStorages: queue a global task on the DOM manipulation task source given remoteStorage's relevant
    //    global object to fire an event named storage at remoteStorage's relevant global object, using StorageEvent, with key initialized
    //    to key, oldValue initialized to oldValue, newValue initialized to newValue, url initialized to url, and storageArea initialized to
    //    remoteStorage.
    for (auto remote_storage : remote_storages) {
        queue_global_task(Task::Source::DOMManipulation, relevant_global, GC::create_function(heap(), [&realm, key, old_value, new_value, url, remote_storage] {
            StorageEventInit init;
            init.key = move(key);
            init.old_value = move(old_value);
            init.new_value = move(new_value);
            init.url = move(url);
            init.storage_area = remote_storage;
            as<Window>(relevant_global_object(remote_storage)).dispatch_event(StorageEvent::create(realm, EventNames::storage, init));
        }));
    }
}

Vector<FlyString> Storage::supported_property_names() const
{
    // The supported property names on a Storage object storage are the result of running get the keys on storage's map.
    Vector<FlyString> names;
    auto keys = m_storage_bottle->keys();
    names.ensure_capacity(keys.size());
    for (auto const& key : keys)
        names.unchecked_append(key);
    return names;
}

Optional<JS::Value> Storage::item_value(size_t index) const
{
    // Handle index as a string since that's our key type
    auto key = String::number(index);
    auto value = get_item(key);
    if (!value.has_value())
        return {};
    return JS::PrimitiveString::create(vm(), value.release_value());
}

JS::Value Storage::named_item_value(FlyString const& name) const
{
    auto value = get_item(String(name));
    if (!value.has_value())
        // AD-HOC: Spec leaves open to a description at: https://html.spec.whatwg.org/multipage/webstorage.html#the-storage-interface
        // However correct behavior expected here: https://github.com/whatwg/html/issues/8684
        return JS::js_undefined();
    return JS::PrimitiveString::create(vm(), value.release_value());
}

WebIDL::ExceptionOr<Bindings::PlatformObject::DidDeletionFail> Storage::delete_value(String const& name)
{
    remove_item(name);
    return DidDeletionFail::NotRelevant;
}

WebIDL::ExceptionOr<void> Storage::set_value_of_indexed_property(u32 index, JS::Value unconverted_value)
{
    // Handle index as a string since that's our key type
    auto key = String::number(index);
    return set_value_of_named_property(key, unconverted_value);
}

WebIDL::ExceptionOr<void> Storage::set_value_of_named_property(String const& key, JS::Value unconverted_value)
{
    // NOTE: Since PlatformObject does not know the type of value, we must convert it ourselves.
    //       The type of `value` is `DOMString`.
    auto value = TRY(unconverted_value.to_string(vm()));
    return set_item(key, value);
}

void Storage::dump() const
{
    auto keys = m_storage_bottle->keys();
    dbgln("Storage ({} key(s))", keys.size());
    size_t i = 0;
    for (auto const& key : keys) {
        auto value = m_storage_bottle->get(key);
        dbgln("[{}] \"{}\": \"{}\"", i, key, value.value());
        ++i;
    }
}

}
