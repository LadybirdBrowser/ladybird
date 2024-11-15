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
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 9.3 Realms, https://tc39.es/ecma262/#realm-record
class Realm final : public Cell {
    GC_CELL(Realm, Cell);
    GC_DECLARE_ALLOCATOR(Realm);

public:
    struct HostDefined {
        virtual ~HostDefined() = default;

        virtual void visit_edges(Cell::Visitor&) { }
    };

    template<typename T, typename... Args>
    GC::Ref<T> create(Args&&... args)
    {
        auto object = heap().allocate<T>(forward<Args>(args)...);
        static_cast<Cell*>(object)->initialize(*this);
        return *object;
    }

    static ThrowCompletionOr<NonnullOwnPtr<ExecutionContext>> initialize_host_defined_realm(VM&, Function<Object*(Realm&)> create_global_object, Function<Object*(Realm&)> create_global_this_value);

    [[nodiscard]] Object& global_object() const { return *m_global_object; }
    void set_global_object(GC::Ref<Object> global) { m_global_object = global; }

    [[nodiscard]] GlobalEnvironment& global_environment() const { return *m_global_environment; }
    void set_global_environment(GC::Ref<GlobalEnvironment> environment) { m_global_environment = environment; }

    [[nodiscard]] Intrinsics const& intrinsics() const { return *m_intrinsics; }
    [[nodiscard]] Intrinsics& intrinsics() { return *m_intrinsics; }
    void set_intrinsics(Badge<Intrinsics>, Intrinsics& intrinsics)
    {
        VERIFY(!m_intrinsics);
        m_intrinsics = &intrinsics;
    }

    HostDefined* host_defined() { return m_host_defined; }
    HostDefined const* host_defined() const { return m_host_defined; }

    void set_host_defined(OwnPtr<HostDefined> host_defined) { m_host_defined = move(host_defined); }

    void define_builtin(Bytecode::Builtin builtin, GC::Ref<NativeFunction> value)
    {
        m_builtins[to_underlying(builtin)] = value;
    }

    GC::Ref<NativeFunction> get_builtin_value(Bytecode::Builtin builtin)
    {
        return *m_builtins[to_underlying(builtin)];
    }

private:
    Realm() = default;

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<Intrinsics> m_intrinsics;                // [[Intrinsics]]
    GC::Ptr<Object> m_global_object;                 // [[GlobalObject]]
    GC::Ptr<GlobalEnvironment> m_global_environment; // [[GlobalEnv]]
    OwnPtr<HostDefined> m_host_defined;              // [[HostDefined]]
    AK::Array<GC::Ptr<NativeFunction>, to_underlying(Bytecode::Builtin::__Count)> m_builtins;
};

}
