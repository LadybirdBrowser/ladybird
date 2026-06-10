/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/MutationObserver.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/MutationRecord.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

struct SimilarOriginWindowAgent;

}

namespace Web::DOM {

struct MutationObserverOptions {
    Optional<Vector<String>> attribute_filter;
    Optional<bool> attribute_old_value;
    Optional<bool> attributes;
    Optional<bool> character_data;
    Optional<bool> character_data_old_value;
    bool child_list { false };
    bool subtree { false };
};

// https://dom.spec.whatwg.org/#mutationobserver
class MutationObserver final : public Bindings::Wrappable {
    WEB_WRAPPABLE(MutationObserver, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MutationObserver);

public:
    static WebIDL::ExceptionOr<GC::Ref<MutationObserver>> create(GC::Ptr<WebIDL::CallbackType>);
    virtual ~MutationObserver() override;

    WebIDL::ExceptionOr<void> observe(Node& target, MutationObserverOptions options);
    WebIDL::ExceptionOr<void> observe(Node& target, Bindings::MutationObserverInit const& options);
    void disconnect();
    Vector<GC::Root<MutationRecord>> take_records();

    Vector<GC::Weak<Node>>& node_list() { return m_node_list; }
    Vector<GC::Weak<Node>> const& node_list() const { return m_node_list; }

    WebIDL::CallbackType& callback() { return *m_callback; }

    void enqueue_record(Badge<Node>, GC::Ref<MutationRecord> mutation_record)
    {
        m_record_queue.append(*mutation_record);
    }

private:
    explicit MutationObserver(GC::Ptr<WebIDL::CallbackType>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://dom.spec.whatwg.org/#concept-mo-callback
    GC::Ptr<WebIDL::CallbackType> m_callback;

    // https://dom.spec.whatwg.org/#mutationobserver-node-list
    // Registered observers in a node’s registered observer list have a weak reference to the node.
    Vector<GC::Weak<Node>> m_node_list;

    // https://dom.spec.whatwg.org/#concept-mo-queue
    Vector<GC::Ref<MutationRecord>> m_record_queue;
};

// https://dom.spec.whatwg.org/#registered-observer
class RegisteredObserver : public JS::Cell {
    GC_CELL(RegisteredObserver, JS::Cell);
    GC_DECLARE_ALLOCATOR(RegisteredObserver);

public:
    static GC::Ref<RegisteredObserver> create(MutationObserver&, MutationObserverOptions const&);
    virtual ~RegisteredObserver() override;

    virtual bool is_transient() const { return false; }

    GC::Ref<MutationObserver> observer() const { return m_observer; }

    MutationObserverOptions const& options() const { return m_options; }
    void set_options(MutationObserverOptions options) { m_options = move(options); }

    template<typename T>
    bool fast_is() const = delete;

protected:
    RegisteredObserver(MutationObserver& observer, MutationObserverOptions const& options);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<MutationObserver> m_observer;
    MutationObserverOptions m_options;
};

// https://dom.spec.whatwg.org/#transient-registered-observer
class TransientRegisteredObserver final : public RegisteredObserver {
    GC_CELL(TransientRegisteredObserver, RegisteredObserver);
    GC_DECLARE_ALLOCATOR(TransientRegisteredObserver);

public:
    static GC::Ref<TransientRegisteredObserver> create(MutationObserver&, MutationObserverOptions const&, RegisteredObserver& source);
    virtual ~TransientRegisteredObserver() override;

    GC::Ref<RegisteredObserver> source() const { return m_source; }

    virtual bool is_transient() const override { return true; }

private:
    TransientRegisteredObserver(MutationObserver& observer, MutationObserverOptions const& options, RegisteredObserver& source);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<RegisteredObserver> m_source;
};

template<>
inline bool RegisteredObserver::fast_is<TransientRegisteredObserver>() const { return is_transient(); }

WEB_API void queue_mutation_observer_microtask(HTML::SimilarOriginWindowAgent&);

}
