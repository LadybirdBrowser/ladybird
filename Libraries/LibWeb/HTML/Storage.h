/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webstorage.html#storage-2
class WEB_API Storage : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Storage, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Storage);

public:
    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-type
    enum class Type {
        Local,
        Session,
    };

    [[nodiscard]] static GC::Ref<Storage> create(JS::Realm&, Type, GC::Ref<StorageAPI::StorageBottle>);

    ~Storage();

    size_t length() const;
    Optional<Utf16String> key(size_t index);
    Optional<Utf16String> get_item(Utf16View key) const;
    WebIDL::ExceptionOr<void> set_item(Utf16View key, Utf16View value);
    void remove_item(Utf16View key);
    void clear();
    Type type() const { return m_type; }

    void dump() const;

private:
    Storage(JS::Realm&, Type, GC::Ref<StorageAPI::StorageBottle>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    // ^PlatformObject
    virtual JS::Value named_item_value(Utf16FlyString const&) const override;
    virtual WebIDL::ExceptionOr<DidDeletionFail> delete_value(Utf16FlyString const&) override;
    virtual Vector<Utf16FlyString> supported_property_names() const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_named_property(Utf16FlyString const& key, JS::Value value) override;

    void reorder();
    void broadcast(Optional<Utf16View> key, Optional<Utf16View> old_value, Optional<Utf16View> new_value);

    Type m_type {};
    GC::Ref<StorageAPI::StorageBottle> m_storage_bottle;
};

}
