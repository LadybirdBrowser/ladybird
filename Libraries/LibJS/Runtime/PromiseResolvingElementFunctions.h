/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>

namespace JS {

struct RemainingElements final : public Cell {
    JS_CELL(RemainingElements, Cell);
    JS_DECLARE_ALLOCATOR(RemainingElements);

    u64 value { 0 };

private:
    RemainingElements() = default;

    explicit RemainingElements(u64 initial_value)
        : value(initial_value)
    {
    }
};

class PromiseValueList final : public Cell {
    JS_CELL(PromiseValueList, Cell);
    JS_DECLARE_ALLOCATOR(PromiseValueList);

public:
    Vector<Value>& values() { return m_values; }
    Vector<Value> const& values() const { return m_values; }

private:
    PromiseValueList() = default;

    virtual void visit_edges(Visitor&) override;

    Vector<Value> m_values;
};

class PromiseResolvingElementFunction : public NativeFunction {
    JS_OBJECT(PromiseResolvingElementFunction, NativeFunction);
    JS_DECLARE_ALLOCATOR(PromiseResolvingElementFunction);

public:
    virtual void initialize(Realm&) override;
    virtual ~PromiseResolvingElementFunction() override = default;

    virtual ThrowCompletionOr<Value> call() override;

protected:
    explicit PromiseResolvingElementFunction(size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&, Object& prototype);

    virtual ThrowCompletionOr<Value> resolve_element() = 0;

    size_t m_index { 0 };
    NonnullGCPtr<PromiseValueList> m_values;
    NonnullGCPtr<PromiseCapability const> m_capability;
    NonnullGCPtr<RemainingElements> m_remaining_elements;

private:
    virtual void visit_edges(Visitor&) override;

    bool m_already_called { false };
};

// 27.2.4.1.3 Promise.all Resolve Element Functions, https://tc39.es/ecma262/#sec-promise.all-resolve-element-functions
class PromiseAllResolveElementFunction final : public PromiseResolvingElementFunction {
    JS_OBJECT(PromiseAllResolveElementFunction, PromiseResolvingElementFunction);
    JS_DECLARE_ALLOCATOR(PromiseAllResolveElementFunction);

public:
    static NonnullGCPtr<PromiseAllResolveElementFunction> create(Realm&, size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&);

    virtual ~PromiseAllResolveElementFunction() override = default;

private:
    explicit PromiseAllResolveElementFunction(size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&, Object& prototype);

    virtual ThrowCompletionOr<Value> resolve_element() override;
};

// 27.2.4.2.2 Promise.allSettled Resolve Element Functions, https://tc39.es/ecma262/#sec-promise.allsettled-resolve-element-functions
class PromiseAllSettledResolveElementFunction final : public PromiseResolvingElementFunction {
    JS_OBJECT(PromiseAllSettledResolveElementFunction, PromiseResolvingElementFunction);
    JS_DECLARE_ALLOCATOR(PromiseAllSettledResolveElementFunction);

public:
    static NonnullGCPtr<PromiseAllSettledResolveElementFunction> create(Realm&, size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&);

    virtual ~PromiseAllSettledResolveElementFunction() override = default;

private:
    explicit PromiseAllSettledResolveElementFunction(size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&, Object& prototype);

    virtual ThrowCompletionOr<Value> resolve_element() override;
};

// 27.2.4.2.3 Promise.allSettled Reject Element Functions, https://tc39.es/ecma262/#sec-promise.allsettled-reject-element-functions
class PromiseAllSettledRejectElementFunction final : public PromiseResolvingElementFunction {
    JS_OBJECT(PromiseAllSettledRejectElementFunction, PromiseResolvingElementFunction);
    JS_DECLARE_ALLOCATOR(PromiseAllSettledRejectElementFunction);

public:
    static NonnullGCPtr<PromiseAllSettledRejectElementFunction> create(Realm&, size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&);

    virtual ~PromiseAllSettledRejectElementFunction() override = default;

private:
    explicit PromiseAllSettledRejectElementFunction(size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&, Object& prototype);

    virtual ThrowCompletionOr<Value> resolve_element() override;
};

// 27.2.4.3.2 Promise.any Reject Element Functions, https://tc39.es/ecma262/#sec-promise.any-reject-element-functions
class PromiseAnyRejectElementFunction final : public PromiseResolvingElementFunction {
    JS_OBJECT(PromiseAnyRejectElementFunction, PromiseResolvingElementFunction);
    JS_DECLARE_ALLOCATOR(PromiseAnyRejectElementFunction);

public:
    static NonnullGCPtr<PromiseAnyRejectElementFunction> create(Realm&, size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&);

    virtual ~PromiseAnyRejectElementFunction() override = default;

private:
    explicit PromiseAnyRejectElementFunction(size_t, PromiseValueList&, NonnullGCPtr<PromiseCapability const>, RemainingElements&, Object& prototype);

    virtual ThrowCompletionOr<Value> resolve_element() override;
};

}
