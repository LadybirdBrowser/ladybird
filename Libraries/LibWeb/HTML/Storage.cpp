/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StoragePrototype.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/StorageEvent.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Storage);

static HashTable<GC::RawRef<Storage>>& all_storages()
{
    // FIXME: This needs to be stored at the user agent level.
    static HashTable<GC::RawRef<Storage>> storages;
    return storages;
}

GC::Ref<Storage> Storage::create(JS::Realm& realm, Type type, NonnullRefPtr<StorageAPI::StorageBottle> storage_bottle)
{
    return realm.create<Storage>(realm, type, move(storage_bottle));
}

Storage::Storage(JS::Realm& realm, Type type, NonnullRefPtr<StorageAPI::StorageBottle> storage_bottle)
    : Bindings::PlatformObject(realm)
    , m_type(type)
    , m_storage_bottle(move(storage_bottle))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Storage);
}

void Storage::finalize()
{
    all_storages().remove(*this);
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-length
size_t Storage::length() const
{
    // The length getter steps are to return this's map's size.
    return map().size();
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-key
Optional<String> Storage::key(size_t index)
{
    // 1. If index is greater than or equal to this's map's size, then return null.
    if (index >= map().size())
        return {};

    // 2. Let keys be the result of running get the keys on this's map.
    auto keys = map().keys();

    // 3. Return keys[index].
    return keys[index];
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-getitem
Optional<String> Storage::get_item(StringView key) const
{
    // 1. If this's map[key] does not exist, then return null.
    auto it = map().find(key);
    if (it == map().end())
        return {};

    // 2. Return this's map[key].
    return it->value;
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-setitem
WebIDL::ExceptionOr<void> Storage::set_item(String const& key, String const& value)
{
    auto& realm = this->realm();

    // 1. Let oldValue be null.
    Optional<String> old_value;

    // 2. Let reorder be true.
    bool reorder = true;

    // 3. If this's map[key] exists:
    auto new_size = m_stored_bytes;
    if (auto it = map().find(key); it != map().end()) {
        // 1. Set oldValue to this's map[key].
        old_value = it->value;

        // 2. If oldValue is value, then return.
        if (old_value == value)
            return {};

        // 3. Set reorder to false.
        reorder = false;
    } else {
        new_size += key.bytes().size();
    }

    // 4. If value cannot be stored, then throw a "QuotaExceededError" DOMException exception.
    new_size += value.bytes().size() - old_value.value_or(String {}).bytes().size();
    if (m_storage_bottle->quota.has_value() && new_size > *m_storage_bottle->quota)
        return WebIDL::QuotaExceededError::create(realm, MUST(String::formatted("Unable to store more than {} bytes in storage"sv, *m_storage_bottle->quota)));

    // 5. Set this's map[key] to value.
    map().set(key, value);
    m_stored_bytes = new_size;

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
    auto it = map().find(key);
    if (it == map().end())
        return;

    // 2. Set oldValue to this's map[key].
    auto old_value = it->value;

    // 3. Remove this's map[key].
    map().remove(it);
    m_stored_bytes = m_stored_bytes - key.bytes().size() - old_value.bytes().size();

    // 4. Reorder this.
    reorder();

    // 5. Broadcast this with key, oldValue, and null.
    broadcast(key, old_value, {});
}

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-clear
void Storage::clear()
{
    // 1. Clear this's map.
    map().clear();

    // 2. Broadcast this with null, null, and null.
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

        // * relevant settings object's origin is same origin with storage's relevant settings object's origin.
        if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(storage).origin()))
            continue;

        // * and, if type is "session", whose relevant settings object's associated Document's node navigable's traversable navigable
        //   is thisDocument's node navigable's traversable navigable.
        if (type() == Type::Session) {
            auto& storage_document = *relevant_settings_object(storage).responsible_document();

            // NOTE: It is possible the remote storage may have not been fully teared down immediately at the point it's document is made inactive.
            if (!storage_document.navigable())
                continue;
            VERIFY(this_document.navigable());

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
    names.ensure_capacity(map().size());
    for (auto const& key : map().keys())
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
    auto value = get_item(name);
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
    dbgln("Storage ({} key(s))", map().size());
    size_t i = 0;
    for (auto const& it : map()) {
        dbgln("[{}] \"{}\": \"{}\"", i, it.key, it.value);
        ++i;
    }
}

}
