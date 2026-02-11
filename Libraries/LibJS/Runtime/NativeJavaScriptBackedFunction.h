/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>

namespace JS {

class NativeJavaScriptBackedFunction final : public NativeFunction {
    JS_OBJECT(NativeJavaScriptBackedFunction, NativeFunction);
    GC_DECLARE_ALLOCATOR(NativeJavaScriptBackedFunction);

public:
    static GC::Ref<NativeJavaScriptBackedFunction> create(Realm&, GC::Ref<SharedFunctionInstanceData>, PropertyKey const& name, i32 length);

    virtual ~NativeJavaScriptBackedFunction() override = default;

    virtual void visit_edges(Visitor&) override;

    virtual void get_stack_frame_size(size_t& registers_and_locals_count, size_t& constants_count, size_t& argument_count) override;

    virtual ThrowCompletionOr<Value> call() override;

    Bytecode::Executable& bytecode_executable();
    FunctionKind kind() const;
    ThisMode this_mode() const;

    virtual bool function_environment_needed() const override;
    virtual size_t function_environment_bindings_count() const override;
    virtual bool is_strict_mode() const override;

private:
    explicit NativeJavaScriptBackedFunction(GC::Ref<SharedFunctionInstanceData> shared_function_instance_data, Object& prototype);

    GC::Ref<SharedFunctionInstanceData> m_shared_function_instance_data;
};

}
