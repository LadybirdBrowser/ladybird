/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::IndexedDB {

class IDBRequest : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBRequest, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(IDBRequest);

public:
    virtual ~IDBRequest() override;

    [[nodiscard]] JS::Value result() const { return m_result; }
    [[nodiscard]] JS::GCPtr<WebIDL::DOMException> error() const { return m_error; }
    [[nodiscard]] bool done() const { return m_done; }
    [[nodiscard]] bool processed() const { return m_processed; }

    void set_done(bool done) { m_done = done; }
    void set_result(JS::Value result) { m_result = result; }
    void set_error(JS::GCPtr<WebIDL::DOMException> error) { m_error = error; }
    void set_processed(bool processed) { m_processed = processed; }

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
    JS::GCPtr<WebIDL::DOMException> m_error;
    // FIXME: A request has a source object.
    // FIXME: A request has a transaction which is initially null.
};

}
