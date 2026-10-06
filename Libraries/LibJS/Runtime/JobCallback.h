/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

// 9.5.1 JobCallback Records, https://tc39.es/ecma262/#sec-jobcallback-records
class JS_API JobCallback final : public EngineCell {
public:
    [[nodiscard]] static GC::Ref<JobCallback> create(VM&, FunctionObject& callback, GC::Ptr<GC::Cell> custom_data);

    FunctionObject& callback();
    GC::Ptr<GC::Cell> custom_data() const;
};

}
