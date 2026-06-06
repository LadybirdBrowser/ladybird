/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Storage.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class Window;

// https://html.spec.whatwg.org/multipage/webstorage.html#storage-2
class WEB_API Storage : public Bindings::Wrappable {
    WEB_WRAPPABLE(Storage, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Storage);

public:
    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-type
    enum class Type {
        Local,
        Session,
    };

    [[nodiscard]] static GC::Ref<Storage> create(Window&, Type, GC::Ref<StorageAPI::StorageBottle>);

    ~Storage();

    size_t length() const;
    Optional<String> key(size_t index);
    Optional<String> get_item(String const& key) const;
    WebIDL::ExceptionOr<void> set_item(JS::Realm&, String const& key, String const& value);
    void remove_item(String const& key);
    void clear();
    Type type() const { return m_type; }

    void dump() const;

private:
    Storage(Window&, Type, GC::Ref<StorageAPI::StorageBottle>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // ^Wrappable
    virtual JS::Value named_item_value(Bindings::WrapperWorld&, JS::Realm&, FlyString const&) const override;
    virtual WebIDL::ExceptionOr<Bindings::NamedPropertyDeletionResult> delete_value(JS::Realm&, String const&) override;
    virtual Vector<FlyString> supported_property_names() const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_named_property(JS::Realm&, String const& key, JS::Value value) override;

    void reorder();
    void broadcast(Optional<String> const& key, Optional<String> const& old_value, Optional<String> const& new_value);

    GC::Ref<Window> m_window;
    Type m_type {};
    GC::Ref<StorageAPI::StorageBottle> m_storage_bottle;
};

}
