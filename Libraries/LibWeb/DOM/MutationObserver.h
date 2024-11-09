/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/DOM/MutationRecord.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#dictdef-mutationobserverinit
struct MutationObserverInit {
    bool child_list { false };
    Optional<bool> attributes;
    Optional<bool> character_data;
    bool subtree { false };
    Optional<bool> attribute_old_value;
    Optional<bool> character_data_old_value;
    Optional<Vector<String>> attribute_filter;
};

// https://dom.spec.whatwg.org/#mutationobserver
class MutationObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MutationObserver, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(MutationObserver);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<MutationObserver>> construct_impl(JS::Realm&, JS::GCPtr<WebIDL::CallbackType>);
    virtual ~MutationObserver() override;

    WebIDL::ExceptionOr<void> observe(Node& target, MutationObserverInit options = {});
    void disconnect();
    Vector<JS::Handle<MutationRecord>> take_records();

    Vector<WeakPtr<Node>>& node_list() { return m_node_list; }
    Vector<WeakPtr<Node>> const& node_list() const { return m_node_list; }

    WebIDL::CallbackType& callback() { return *m_callback; }

    void enqueue_record(Badge<Node>, JS::NonnullGCPtr<MutationRecord> mutation_record)
    {
        m_record_queue.append(*mutation_record);
    }

private:
    MutationObserver(JS::Realm&, JS::GCPtr<WebIDL::CallbackType>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://dom.spec.whatwg.org/#concept-mo-callback
    JS::GCPtr<WebIDL::CallbackType> m_callback;

    // https://dom.spec.whatwg.org/#mutationobserver-node-list
    // NOTE: These are weak, per https://dom.spec.whatwg.org/#garbage-collection
    // Registered observers in a node’s registered observer list have a weak reference to the node.
    Vector<WeakPtr<Node>> m_node_list;

    // https://dom.spec.whatwg.org/#concept-mo-queue
    Vector<JS::NonnullGCPtr<MutationRecord>> m_record_queue;
};

// https://dom.spec.whatwg.org/#registered-observer
class RegisteredObserver : public JS::Cell {
    JS_CELL(RegisteredObserver, JS::Cell);

public:
    static JS::NonnullGCPtr<RegisteredObserver> create(MutationObserver&, MutationObserverInit const&);
    virtual ~RegisteredObserver() override;

    JS::NonnullGCPtr<MutationObserver> observer() const { return m_observer; }

    MutationObserverInit const& options() const { return m_options; }
    void set_options(MutationObserverInit options) { m_options = move(options); }

protected:
    RegisteredObserver(MutationObserver& observer, MutationObserverInit const& options);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    JS::NonnullGCPtr<MutationObserver> m_observer;
    MutationObserverInit m_options;
};

// https://dom.spec.whatwg.org/#transient-registered-observer
class TransientRegisteredObserver final : public RegisteredObserver {
    JS_CELL(TransientRegisteredObserver, RegisteredObserver);
    JS_DECLARE_ALLOCATOR(TransientRegisteredObserver);

public:
    static JS::NonnullGCPtr<TransientRegisteredObserver> create(MutationObserver&, MutationObserverInit const&, RegisteredObserver& source);
    virtual ~TransientRegisteredObserver() override;

    JS::NonnullGCPtr<RegisteredObserver> source() const { return m_source; }

private:
    TransientRegisteredObserver(MutationObserver& observer, MutationObserverInit const& options, RegisteredObserver& source);

    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<RegisteredObserver> m_source;
};

}
