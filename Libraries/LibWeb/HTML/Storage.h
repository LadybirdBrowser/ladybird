/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webstorage.html#storage-2
class Storage : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Storage, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Storage);

public:
    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-type
    enum class Type {
        Local,
        Session,
    };

    [[nodiscard]] static GC::Ref<Storage> create(JS::Realm&, Type, u64 quota_bytes);

    ~Storage();

    size_t length() const;
    Optional<String> key(size_t index);
    Optional<String> get_item(StringView key) const;
    WebIDL::ExceptionOr<void> set_item(String const& key, String const& value);
    void remove_item(StringView key);
    void clear();

    auto const& map() const { return m_map; }
    Type type() const { return m_type; }

    void dump() const;

private:
    Storage(JS::Realm&, Type, u64 quota_limit);

    virtual void initialize(JS::Realm&) override;

    // ^PlatformObject
    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual JS::Value named_item_value(FlyString const&) const override;
    virtual WebIDL::ExceptionOr<DidDeletionFail> delete_value(String const&) override;
    virtual Vector<FlyString> supported_property_names() const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_indexed_property(u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_named_property(String const& key, JS::Value value) override;

    void reorder();
    void broadcast(StringView key, StringView old_value, StringView new_value);

    OrderedHashMap<String, String> m_map;
    Type m_type {};
    u64 m_quota_bytes { 0 };
    u64 m_stored_bytes { 0 };
};

}
