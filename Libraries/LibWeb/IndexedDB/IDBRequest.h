/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/IDBRequestPrototype.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::IndexedDB {

using IDBRequestSource = Variant<Empty, GC::Ref<IDBObjectStore>, GC::Ref<IDBIndex>, GC::Ref<IDBCursor>>;

class IDBRequest : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBRequest, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(IDBRequest);

public:
    virtual ~IDBRequest() override;

    [[nodiscard]] JS::Value result() const { return m_result; }
    [[nodiscard]] GC::Ptr<WebIDL::DOMException> error() const { return m_error; }
    [[nodiscard]] bool done() const { return m_done; }
    [[nodiscard]] bool processed() const { return m_processed; }
    [[nodiscard]] IDBRequestSource source() const { return m_source; }

    [[nodiscard]] Bindings::IDBRequestReadyState ready_state() const;

    void set_done(bool done) { m_done = done; }
    void set_result(JS::Value result) { m_result = result; }
    void set_error(GC::Ptr<WebIDL::DOMException> error) { m_error = error; }
    void set_processed(bool processed) { m_processed = processed; }
    void set_source(IDBRequestSource source) { m_source = source; }

    void set_onsuccess(WebIDL::CallbackType*);
    WebIDL::CallbackType* onsuccess();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

protected:
    explicit IDBRequest(JS::Realm&);

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
    // FIXME: A request has a transaction which is initially null.
};

}
