/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/IDBDatabasePrototype.h>
#include <LibWeb/Bindings/IDBTransactionPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>
#include <LibWeb/IndexedDB/Internal/RequestList.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#transaction
class IDBTransaction : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBTransaction, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(IDBTransaction);

    enum TransactionState {
        Active,
        Inactive,
        Committing,
        Finished
    };

public:
    virtual ~IDBTransaction() override;

    [[nodiscard]] static GC::Ref<IDBTransaction> create(JS::Realm&, GC::Ref<IDBDatabase>, Bindings::IDBTransactionMode, Bindings::IDBTransactionDurability, Vector<GC::Ref<ObjectStore>>);
    [[nodiscard]] Bindings::IDBTransactionMode mode() const { return m_mode; }
    [[nodiscard]] TransactionState state() const { return m_state; }
    [[nodiscard]] GC::Ptr<WebIDL::DOMException> error() const { return m_error; }
    [[nodiscard]] GC::Ref<IDBDatabase> connection() const { return m_connection; }
    [[nodiscard]] Bindings::IDBTransactionDurability durability() const { return m_durability; }
    [[nodiscard]] GC::Ptr<IDBRequest> associated_request() const { return m_associated_request; }
    [[nodiscard]] bool aborted() const { return m_aborted; }
    [[nodiscard]] GC::Ref<HTML::DOMStringList> object_store_names();
    [[nodiscard]] RequestList& request_list() { return m_request_list; }
    [[nodiscard]] ReadonlySpan<GC::Ref<ObjectStore>> scope() const { return m_scope; }
    [[nodiscard]] String uuid() const { return m_uuid; }
    [[nodiscard]] GC::Ptr<HTML::EventLoop> cleanup_event_loop() const { return m_cleanup_event_loop; }

    void set_mode(Bindings::IDBTransactionMode mode) { m_mode = mode; }
    void set_error(GC::Ptr<WebIDL::DOMException> error) { m_error = error; }
    void set_associated_request(GC::Ptr<IDBRequest> request) { m_associated_request = request; }
    void set_aborted(bool aborted) { m_aborted = aborted; }
    void set_cleanup_event_loop(GC::Ptr<HTML::EventLoop> event_loop) { m_cleanup_event_loop = event_loop; }
    void set_state(TransactionState state) { m_state = state; }

    [[nodiscard]] bool is_upgrade_transaction() const { return m_mode == Bindings::IDBTransactionMode::Versionchange; }
    [[nodiscard]] bool is_readonly() const { return m_mode == Bindings::IDBTransactionMode::Readonly; }
    [[nodiscard]] bool is_readwrite() const { return m_mode == Bindings::IDBTransactionMode::Readwrite; }
    [[nodiscard]] bool is_finished() const { return m_state == TransactionState::Finished; }
    [[nodiscard]] bool is_active() const { return m_state == TransactionState::Active; }
    [[nodiscard]] bool is_inactive() const { return m_state == TransactionState::Inactive; }
    [[nodiscard]] bool is_committing() const { return m_state == TransactionState::Committing; }

    GC::Ptr<ObjectStore> object_store_named(String const& name) const;
    void add_to_scope(GC::Ref<ObjectStore> object_store) { m_scope.append(object_store); }

    WebIDL::ExceptionOr<void> abort();
    WebIDL::ExceptionOr<void> commit();
    WebIDL::ExceptionOr<GC::Ref<IDBObjectStore>> object_store(String const& name);

    void set_onabort(WebIDL::CallbackType*);
    WebIDL::CallbackType* onabort();
    void set_oncomplete(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncomplete();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

protected:
    explicit IDBTransaction(JS::Realm&, GC::Ref<IDBDatabase>, Bindings::IDBTransactionMode, Bindings::IDBTransactionDurability, Vector<GC::Ref<ObjectStore>>);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // AD-HOC: The transaction has a connection
    GC::Ref<IDBDatabase> m_connection;

    // A transaction has a mode that determines which types of interactions can be performed upon that transaction.
    Bindings::IDBTransactionMode m_mode;

    // A transaction has a durability hint. This is a hint to the user agent of whether to prioritize performance or durability when committing the transaction.
    Bindings::IDBTransactionDurability m_durability { Bindings::IDBTransactionDurability::Default };

    // A transaction has a state
    TransactionState m_state;

    // A transaction has a error which is set if the transaction is aborted.
    GC::Ptr<WebIDL::DOMException> m_error;

    // A transaction has an associated upgrade request
    GC::Ptr<IDBRequest> m_associated_request;

    // AD-HOC: We need to track abort state separately, since we cannot rely on only the error.
    bool m_aborted { false };

    // A transaction has a scope which is a set of object stores that the transaction may interact with.
    Vector<GC::Ref<ObjectStore>> m_scope;

    // A transaction has a request list of pending requests which have been made against the transaction.
    RequestList m_request_list;

    // A transaction optionally has a cleanup event loop which is an event loop.
    GC::Ptr<HTML::EventLoop> m_cleanup_event_loop;

    // NOTE: Used for debug purposes
    String m_uuid;
};

}
