/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class Window;

// https://html.spec.whatwg.org/multipage/webstorage.html#storage-2
class WEB_API Storage : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Storage, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Storage);

public:
    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-type
    enum class Type {
        Local,
        Session,
    };

    [[nodiscard]] static GC::Ref<Storage> create(Window&, Type, GC::Ref<StorageAPI::StorageBottle>);

    ~Storage();

    Window& window() { return m_window; }
    Window const& window() const { return m_window; }

    size_t length() const;
    Optional<Utf16String> key(size_t index);
    Optional<Utf16String> get_item(Utf16View key) const;
    WebIDL::ExceptionOr<void> set_item(Utf16View key, Utf16View value);
    void remove_item(Utf16View key);
    void clear();
    Type type() const { return m_type; }

    void dump() const;

    // A window listening for storage events has its storage connected so that changes made in other processes reach
    // it; a map is otherwise only loaded on first access.
    void connect() const { m_storage_bottle->connect(); }

    // Fires the storage events a change calls for on every Storage object in this process that shares the changed
    // map, except excluded_storage: that is the one whose script made the change, when the change was made here.
    // A change another process made has no such object, and every Storage here gets the event.
    static void dispatch_storage_events_for_remote_change(Type, StorageAPI::StorageKey const&, Optional<PageId> session_page, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url, GC::Ptr<Storage> excluded_storage);

private:
    Storage(Window&, Type, GC::Ref<StorageAPI::StorageBottle>);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    // ^Wrappable
    virtual Vector<Utf16FlyString> supported_property_names() const override;
    void reorder();
    void broadcast(Optional<Utf16View> key, Optional<Utf16View> old_value, Optional<Utf16View> new_value, String const& url);

    GC::Ref<Window> m_window;
    Type m_type {};
    GC::Ref<StorageAPI::StorageBottle> m_storage_bottle;
};

}
