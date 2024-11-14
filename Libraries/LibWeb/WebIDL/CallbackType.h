/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::WebIDL {

enum class OperationReturnsPromise {
    Yes,
    No,
};

// https://webidl.spec.whatwg.org/#idl-callback-interface
class CallbackType final : public JS::Cell {
    GC_CELL(CallbackType, JS::Cell);
    GC_DECLARE_ALLOCATOR(CallbackType);

public:
    CallbackType(JS::Object& callback, HTML::EnvironmentSettingsObject& callback_context, OperationReturnsPromise = OperationReturnsPromise::No);

    GC::Ref<JS::Object> callback;

    // https://webidl.spec.whatwg.org/#dfn-callback-context
    GC::Ref<HTML::EnvironmentSettingsObject> callback_context;

    // Non-standard property used to distinguish Promise-returning callbacks in callback-related AOs
    OperationReturnsPromise operation_returns_promise;

private:
    virtual void visit_edges(Cell::Visitor&) override;
};

}
