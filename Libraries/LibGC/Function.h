/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <LibGC/Cell.h>
#include <LibGC/FunctionTable.h>
#include <LibGC/Heap.h>
#include <typeinfo>

namespace GC {

namespace Detail {

// NB: A function is made in one library and called from another, so its signature is the hash of the mangled name of
//     its type, which every compiler and library spells the same. How a compiler spells the type differs: GCC, for one,
//     spells it as it was first written in each source file. Types of the same name with internal linkage, such as
//     those in an anonymous namespace, share one signature.
template<typename T>
inline u64 const function_signature = [] {
    u64 hash = 0xcbf29ce484222325;
    for (auto const* character = typeid(T).name(); *character; ++character) {
        hash ^= static_cast<u8>(*character);
        hash *= 0x100000001b3;
    }
    return hash;
}();

}

template<typename R, typename... Args>
struct FunctionOperationsFor : FunctionOperations {
    R (*invoke)(void* box, Args...) { nullptr };
};

template<typename T>
class Function;

template<typename R, typename... Args>
class Function<R(Args...)> final : public Cell {
    GC_CELL(Function, Cell);
    GC_DECLARE_ALLOCATOR(Function);

public:
    using Operations = FunctionOperationsFor<R, Args...>;
    using Lambda = AK::Function<R(Args...)>;

    static Ref<Function> create(Heap& heap, ESCAPING AK::Function<R(Args...)>&& function)
    {
        // NB: The function is allocated before its box, so a collection that the allocation runs still finds the
        //     captures where the caller holds them.
        auto cell = heap.allocate<Function>();
        auto allocation = heap.function_table().allocate(lambda_operations, sizeof(Lambda));
        new (allocation.box) Lambda(move(function));
        cell->m_handle = allocation.handle;
        return cell;
    }

    R operator()(Args... args) const
    {
        auto const& slot = heap().function_table().slot(m_handle);
        VERIFY(*slot.operations->signature == Detail::function_signature<R(Args...)>);
        // NB: The box is outside the heap, so nothing the callable holds points into this cell. This pointer is kept
        //     live across the call, where the conservative scan of the stack finds it, so that a collection the callable
        //     runs after dropping the last reference to its function does not destroy the box under it.
        ScopeGuard keep_alive = [this] { asm volatile("" : : "r"(this) : "memory"); };
        return static_cast<Operations const&>(*slot.operations).invoke(slot.box, forward<Args>(args)...);
    }

    Function const& function() const { return *this; }

    virtual void finalize() override
    {
        Base::finalize();
        if (!m_handle.is_valid())
            return;
        auto& table = heap().function_table();
        auto const& slot = table.slot(m_handle);
        slot.operations->destroy(slot.box);
        table.free(m_handle);
        m_handle = {};
    }

private:
    Function() = default;

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        if (!m_handle.is_valid())
            return;
        auto const& slot = heap().function_table().slot(m_handle);
        slot.operations->visit_edges(slot.box, visitor);
    }

    // A function made from an AK::Function, whose captures are opaque and visited as possible cell pointers.
    static constexpr Operations lambda_operations {
        {
            .signature = &Detail::function_signature<R(Args...)>,
            .visit_edges = [](void* box, Visitor& visitor) { visitor.visit_possible_values(static_cast<Lambda*>(box)->raw_capture_range()); },
            .destroy = [](void* box) { static_cast<Lambda*>(box)->~Lambda(); },
        },
        [](void* box, Args... args) -> R {
            return (*static_cast<Lambda*>(box))(forward<Args>(args)...);
        },
    };

    FunctionTableHandle m_handle;
};

template<typename Callable, typename T = EquivalentFunctionType<RemoveCVReference<Callable>>>
static Ref<Function<T>> create_function(Heap& heap, ESCAPING Callable&& function)
{
    return Function<T>::create(heap, AK::Function<T> { forward<Callable>(function) });
}

template<typename R, typename... Args>
GC_DEFINE_ALLOCATOR(Function<R(Args...)>);

}
