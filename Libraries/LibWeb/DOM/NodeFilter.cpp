/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/Node.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/NodeIterator.h>
#include <LibWeb/DOM/TreeWalker.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Bindings {

static JS::Value filter_or_null(GC::Ptr<DOM::NodeFilter> filter)
{
    if (!filter)
        return JS::js_null();
    return filter->callback().callback;
}

JS::ThrowCompletionOr<DOM::NodeFilter::Result> accept_node(JS::Realm& realm, DOM::NodeFilter& filter, DOM::Node& node)
{
    auto& callback = filter.callback();
    auto& callback_realm = callback.callback->shape().realm();
    auto wrapped_node = Bindings::wrap(Bindings::host_defined_wrapper_world(callback_realm), callback_realm, GC::Ref { node });
    auto result = WebIDL::call_user_object_operation(callback, "acceptNode"_utf16_fly_string, {}, { { wrapped_node } });
    if (result.is_abrupt())
        return result;

    auto result_value = TRY(result.value().to_i32(realm.vm()));
    return static_cast<DOM::NodeFilter::Result>(result_value);
}

static JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> traversal_result_to_node(JS::Realm& realm, DOM::TraversalResult result, Optional<JS::Completion>& callback_exception, Utf16String const& active_error_message)
{
    switch (result.type) {
    case DOM::TraversalResult::Type::Node:
        return result.node;
    case DOM::TraversalResult::Type::Null:
        return nullptr;
    case DOM::TraversalResult::Type::AlreadyActive:
        return throw_completion(realm, WebIDL::InvalidStateError::create(active_error_message));
    case DOM::TraversalResult::Type::CallbackException:
        VERIFY(callback_exception.has_value());
        return callback_exception.release_value();
    }
    VERIFY_NOT_REACHED();
}

template<typename Traverser, typename Callback>
static JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> traversal(JS::Realm& realm, Traverser& traverser, Callback callback, Utf16String const& active_error_message)
{
    Optional<JS::Completion> callback_exception;
    DOM::TraversalFilter traversal_filter = [&](DOM::Node& node) -> Optional<DOM::NodeFilter::Result> {
        auto result = accept_node(realm, *traverser.filter(), node);
        if (result.is_error()) {
            callback_exception = result.release_error();
            return {};
        }
        return result.release_value();
    };
    return traversal_result_to_node(realm, callback(traverser, traversal_filter), callback_exception, active_error_message);
}

JS::Value filter(JS::Realm&, DOM::NodeIterator& iterator)
{
    return filter_or_null(iterator.filter());
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_node(JS::Realm& realm, DOM::NodeIterator& iterator)
{
    return traversal(
        realm, iterator, [](DOM::NodeIterator& iterator, DOM::TraversalFilter const& traversal_filter) {
            return iterator.next_node(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_node(JS::Realm& realm, DOM::NodeIterator& iterator)
{
    return traversal(
        realm, iterator, [](DOM::NodeIterator& iterator, DOM::TraversalFilter const& traversal_filter) {
            return iterator.previous_node(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::Value filter(JS::Realm&, DOM::TreeWalker& tree_walker)
{
    return filter_or_null(tree_walker.filter());
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> parent_node(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.parent_node(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> first_child(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.first_child(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> last_child(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.last_child(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_sibling(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.previous_sibling(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_sibling(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.next_sibling(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_node(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.previous_node(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_node(JS::Realm& realm, DOM::TreeWalker& tree_walker)
{
    return traversal(
        realm, tree_walker, [](DOM::TreeWalker& tree_walker, DOM::TraversalFilter const& traversal_filter) {
            return tree_walker.next_node(traversal_filter);
        },
        "NodeIterator is already active"_utf16);
}

}

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NodeFilter);

GC::Ref<NodeFilter> NodeFilter::create(GC::Ref<WebIDL::CallbackType> callback)
{
    return GC::Heap::the().allocate<NodeFilter>(callback);
}

NodeFilter::NodeFilter(GC::Ref<WebIDL::CallbackType> callback)
    : m_callback(callback)
{
}

void NodeFilter::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
