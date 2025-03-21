/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/IDBRequestPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>

namespace Web::IndexedDB {

using IDBRequestSource = Variant<Empty, GC::Ref<IDBObjectStore>, GC::Ref<IDBIndex>, GC::Ref<IDBCursor>>;

// https://w3c.github.io/IndexedDB/#request-api
class IDBRequest : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBRequest, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(IDBRequest);

public:
    virtual ~IDBRequest() override;

    [[nodiscard]] static GC::Ref<IDBRequest> create(JS::Realm&, IDBRequestSource);

    [[nodiscard]] bool done() const { return m_done; }
    [[nodiscard]] bool processed() const { return m_processed; }
    [[nodiscard]] IDBRequestSource source() const { return m_source; }
    [[nodiscard]] GC::Ptr<IDBTransaction> transaction() const { return m_transaction; }

    [[nodiscard]] Bindings::IDBRequestReadyState ready_state() const;
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ptr<WebIDL::DOMException>> error() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::Value> result() const;

    void set_done(bool done) { m_done = done; }
    void set_result(JS::Value result) { m_result = result; }
    void set_error(GC::Ptr<WebIDL::DOMException> error) { m_error = error; }
    void set_processed(bool processed) { m_processed = processed; }
    void set_source(IDBRequestSource source) { m_source = source; }
    void set_transaction(GC::Ptr<IDBTransaction> transaction) { m_transaction = transaction; }

    void set_onsuccess(WebIDL::CallbackType*);
    WebIDL::CallbackType* onsuccess();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

protected:
    explicit IDBRequest(JS::Realm&, IDBRequestSource);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // A request has a processed flag which is initially false.
    bool m_processed { false };
    // A request has a done flag which is initially false.
    bool m_done { false };
    // A request has a result and an error
    JS::Value m_result;
    GC::Ptr<WebIDL::DOMException> m_error;
    // A request has a source object.
    IDBRequestSource m_source;
    // A request has a transaction which is initially null.
    GC::Ptr<IDBTransaction> m_transaction;
};

}
