/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Weakable.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 9.3 Realms, https://tc39.es/ecma262/#realm-record
class Realm final : public Cell {
    JS_CELL(Realm, Cell);
    JS_DECLARE_ALLOCATOR(Realm);

public:
    struct HostDefined {
        virtual ~HostDefined() = default;

        virtual void visit_edges(Cell::Visitor&) { }
    };

    static ThrowCompletionOr<NonnullGCPtr<Realm>> create(VM&);
    static ThrowCompletionOr<NonnullOwnPtr<ExecutionContext>> initialize_host_defined_realm(VM&, Function<Object*(Realm&)> create_global_object, Function<Object*(Realm&)> create_global_this_value);

    void set_global_object(Object* global_object, Object* this_value);

    [[nodiscard]] Object& global_object() const { return *m_global_object; }
    [[nodiscard]] GlobalEnvironment& global_environment() const { return *m_global_environment; }

    [[nodiscard]] Intrinsics const& intrinsics() const { return *m_intrinsics; }
    [[nodiscard]] Intrinsics& intrinsics() { return *m_intrinsics; }
    void set_intrinsics(Badge<Intrinsics>, Intrinsics& intrinsics)
    {
        VERIFY(!m_intrinsics);
        m_intrinsics = &intrinsics;
    }

    HostDefined* host_defined() { return m_host_defined; }
    void set_host_defined(OwnPtr<HostDefined> host_defined) { m_host_defined = move(host_defined); }

    void define_builtin(Bytecode::Builtin builtin, NonnullGCPtr<NativeFunction> value)
    {
        m_builtins[to_underlying(builtin)] = value;
    }

    NonnullGCPtr<NativeFunction> get_builtin_value(Bytecode::Builtin builtin)
    {
        return *m_builtins[to_underlying(builtin)];
    }

private:
    Realm() = default;

    virtual void visit_edges(Visitor&) override;

    GCPtr<Intrinsics> m_intrinsics;                // [[Intrinsics]]
    GCPtr<Object> m_global_object;                 // [[GlobalObject]]
    GCPtr<GlobalEnvironment> m_global_environment; // [[GlobalEnv]]
    OwnPtr<HostDefined> m_host_defined;            // [[HostDefined]]
    AK::Array<GCPtr<NativeFunction>, to_underlying(Bytecode::Builtin::__Count)> m_builtins;
};

}
