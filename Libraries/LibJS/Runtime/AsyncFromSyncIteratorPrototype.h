/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/AsyncFromSyncIterator.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

// 27.1.4.2 The %AsyncFromSyncIteratorPrototype% Object, https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%-object
class AsyncFromSyncIteratorPrototype final : public PrototypeObject<AsyncFromSyncIteratorPrototype, AsyncFromSyncIterator> {
    JS_PROTOTYPE_OBJECT(AsyncFromSyncIteratorPrototype, AsyncFromSyncIterator, AsyncFromSyncIterator);
    JS_DECLARE_ALLOCATOR(AsyncFromSyncIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~AsyncFromSyncIteratorPrototype() override = default;

private:
    explicit AsyncFromSyncIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
    JS_DECLARE_NATIVE_FUNCTION(return_);
    JS_DECLARE_NATIVE_FUNCTION(throw_);
};

NonnullGCPtr<IteratorRecord> create_async_from_sync_iterator(VM&, NonnullGCPtr<IteratorRecord> sync_iterator);

}
