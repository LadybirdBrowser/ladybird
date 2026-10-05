/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API FinalizationRegistry final : public Object {
public:
    // CleanupFinalizationRegistry ( finalizationRegistry ), with the callback in place of [[CleanupCallback]] unless it
    // is null.
    ThrowCompletionOr<void> cleanup(GC::Ptr<JobCallback> = {});

    Realm& realm();
    Realm const& realm() const;
    JobCallback& cleanup_callback();
    JobCallback const& cleanup_callback() const;

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_FINALIZATION_REGISTRY; }
};

}
