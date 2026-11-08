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
#include <LibGC/CaptureTraits.h>
#include <LibGC/Cell.h>
#include <LibGC/DeferGC.h>
#include <LibGC/FunctionTable.h>
#include <LibGC/Heap.h>
#include <LibGC/Member.h>
#include <LibGC/Ptr.h>
#include <typeinfo>

namespace GC {

namespace Detail {

// A cell captured by reference is held as a Ref, and handed to the callable as a reference again.
template<typename T>
struct CapturedCellReference {
    Ref<T> cell;

    T& get() { return *cell; }
    void visit_edges(Cell::Visitor& visitor) const { visitor.visit(cell); }
};

// A cell captured by pointer is held as a Ptr, and handed to the callable as a pointer again.
template<typename T>
struct CapturedCellPointer {
    T* cell;

    T*& get() { return cell; }
    void visit_edges(Cell::Visitor& visitor) const { visitor.visit(Ptr<T> { cell }); }
};

template<typename T>
struct CapturedValue {
    static_assert(IsCapturable<T>, "A GC::Function capture must be traced, or hold no cell edges");
    // NB: A value is copied into the function. A polymorphic one may be part of a larger object, such as the mixin of a
    //     cell captured as *this, and the function would act on a copy of it.
    static_assert(!__is_polymorphic(T), "A GC::Function can't capture a polymorphic value, which would be copied out of the object it is part of");

    T value;

    T& get() { return value; }
    void visit_edges(Cell::Visitor& visitor) const
    {
        if constexpr (!IsUntracedCapture<T>::value) {
            static_assert(IsVisitable<T>::value, "A traced GC::Function capture must be visitable");
            visitor.visit(value);
        }
    }
};

// A root that a function captures stops being a root, and is traced through the function instead. A function holding a
// root would keep what the root names alive for as long as the function lives, even through a cycle back to itself.
template<typename T>
struct CapturedRoot {
    CapturedRoot(Root<T> const& root)
        : cell(root.ptr())
    {
    }

    Ptr<T> cell;

    Ptr<T>& get() { return cell; }
    void visit_edges(Cell::Visitor& visitor) const { visitor.visit(cell); }
};

template<typename T, size_t inline_capacity>
struct CapturedRootVector {
    static_assert(IsCapturable<T>, "A GC::Function capture must be traced, or hold no cell edges");

    CapturedRootVector(RootVector<T, inline_capacity>&& vector)
        : elements(move(static_cast<Vector<T, inline_capacity>&>(vector)))
    {
    }

    CapturedRootVector(RootVector<T, inline_capacity> const& vector)
        : elements(vector)
    {
    }

    Vector<T, inline_capacity> elements;

    Vector<T, inline_capacity>& get() { return elements; }
    void visit_edges(Cell::Visitor& visitor) const { visitor.visit(elements); }
};

template<typename T>
struct CapturedRootStorage {
    using Type = void;
};

template<typename T>
struct CapturedRootStorage<Root<T>> {
    using Type = CapturedRoot<T>;
};

// A member of a cell is held as a pointer of its own, so that a function can name it as it is.
template<typename T>
struct CapturedMember {
    CapturedMember(Member<T> const& member)
        : cell(member.ptr())
    {
    }

    Ptr<T> cell;

    Ptr<T>& get() { return cell; }
    void visit_edges(Cell::Visitor& visitor) const { visitor.visit(cell); }
};

template<typename T>
struct CapturedNonnullMember {
    CapturedNonnullMember(NonnullMember<T> const& member)
        : cell(*member)
    {
    }

    Ref<T> cell;

    Ref<T>& get() { return cell; }
    void visit_edges(Cell::Visitor& visitor) const { visitor.visit(cell); }
};

template<typename T>
struct CapturedRootStorage<Member<T>> {
    using Type = CapturedMember<T>;
};

template<typename T>
struct CapturedRootStorage<NonnullMember<T>> {
    using Type = CapturedNonnullMember<T>;
};

template<typename T, size_t inline_capacity>
struct CapturedRootStorage<RootVector<T, inline_capacity>> {
    using Type = CapturedRootVector<T, inline_capacity>;
};

template<typename T>
struct CaptureStorage {
    using Type = CapturedValue<RemoveCVReference<T>>;
};

template<typename T>
requires(!IsSame<typename CapturedRootStorage<RemoveCVReference<T>>::Type, void>)
struct CaptureStorage<T> {
    using Type = typename CapturedRootStorage<RemoveCVReference<T>>::Type;
};

template<typename T>
requires(IsLvalueReference<T> && IsCellLike<RemoveCVReference<T>>)
struct CaptureStorage<T> {
    using Type = CapturedCellReference<RemoveReference<T>>;
};

template<typename T>
requires(IsPointer<RemoveCVReference<T>> && IsCellLike<RemoveCV<RemovePointer<RemoveCVReference<T>>>>)
struct CaptureStorage<T> {
    using Type = CapturedCellPointer<RemovePointer<RemoveCVReference<T>>>;
};

template<size_t index, typename T>
struct CaptureSlot {
    T capture;
};

template<typename Indices, typename... Captures>
struct CaptureBox;

// The captures of a function, outside the GC heap.
template<size_t... indices, typename... Captures>
struct CaptureBox<IndexSequence<indices...>, Captures...> : CaptureSlot<indices, Captures>... {
    template<typename... Values>
    explicit CaptureBox(Values&&... values)
        : CaptureSlot<indices, Captures> { Captures { forward<Values>(values) } }...
    {
    }

    template<typename Callable, typename R, typename... Args>
    R invoke(Args&&... args)
    {
        return Callable {}(static_cast<CaptureSlot<indices, Captures>&>(*this).capture.get()..., forward<Args>(args)...);
    }

    void visit_edges(Cell::Visitor& visitor) const
    {
        (static_cast<CaptureSlot<indices, Captures> const&>(*this).capture.visit_edges(visitor), ...);
    }
};

template<size_t count, typename T>
struct DropParameters;

template<typename R, typename... Args>
struct DropParameters<0, R(Args...)> {
    using Type = R(Args...);
};

template<size_t count, typename R, typename First, typename... Rest>
requires(count > 0)
struct DropParameters<count, R(First, Rest...)> {
    using Type = typename DropParameters<count - 1, R(Rest...)>::Type;
};

// The type of a callable when it is given its captures, which also covers a callable that takes them as auto&.
template<typename Callable, typename... Captures>
struct CallableTypeWithCaptures {
    using Type = EquivalentFunctionType<Callable>;
};

template<typename Callable, typename... Captures>
requires(sizeof...(Captures) > 0 && requires { &Callable::template operator()<RemoveReference<decltype(declval<Captures&>().get())>...>; })
struct CallableTypeWithCaptures<Callable, Captures...> {
    using Type = EquivalentFunctionType<decltype(&Callable::template operator()<RemoveReference<decltype(declval<Captures&>().get())>...>)>;
};

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

template<typename Callable>
concept CapturesNothing = __is_empty(Callable) && IsConstructible<Callable>;

template<typename Indices, typename... Values>
struct CaptureReferences;

template<size_t... indices, typename... Values>
struct CaptureReferences<IndexSequence<indices...>, Values...> : CaptureSlot<indices, Values&&>... {
    explicit CaptureReferences(Values&&... values)
        : CaptureSlot<indices, Values&&> { forward<Values>(values) }...
    {
    }

    template<typename Box>
    void make_box(void* storage)
    {
        new (storage) Box(static_cast<Values&&>(static_cast<CaptureSlot<indices, Values&&>&>(*this).capture)...);
    }
};

}

// What a function captures, as named by GC::captures(). It refers to the values until the function moves or copies them
// into its box, so it lives only as long as the expression that makes the function.
template<typename... Values>
struct CaptureList : Detail::CaptureReferences<MakeIndexSequence<sizeof...(Values)>, Values...> {
    using Detail::CaptureReferences<MakeIndexSequence<sizeof...(Values)>, Values...>::CaptureReferences;
};

template<typename... Values>
CaptureList<Values...> captures(Values&&... values)
{
    return CaptureList<Values...> { forward<Values>(values)... };
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

    template<typename Callable, typename... Values>
    static Ref<Function> create(Heap& heap, CaptureList<Values...>&& captures, Callable&&)
    {
        static_assert(Detail::CapturesNothing<RemoveCVReference<Callable>>, "A GC::Function callable takes what it uses from its captures, and captures nothing itself");
        using Callback = RemoveCVReference<Callable>;
        using Box = Detail::CaptureBox<MakeIndexSequence<sizeof...(Values)>, typename Detail::CaptureStorage<Values>::Type...>;

        static_assert(alignof(Box) <= FunctionTable::box_alignment);

        if constexpr (sizeof...(Values) == 0) {
            auto cell = heap.allocate<Function>();
            cell->m_handle = heap.function_table().allocate(CaptureOperations<Callback, Box>::operations, 0).handle;
            return cell;
        } else {
            // NB: A collection waits until the function holds its box. Until then, the captured values may be where a
            //     conservative scan of the stack doesn't look, such as the storage of a temporary Vector of cells.
            DeferGC defer_gc { heap };
            auto cell = heap.allocate<Function>();
            auto allocation = heap.function_table().allocate(CaptureOperations<Callback, Box>::operations, sizeof(Box));
            captures.template make_box<Box>(allocation.box);
            cell->m_handle = allocation.handle;
            return cell;
        }
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

    // NB: The captured values die when the cell is swept, not when it is finalized, so that the finalizer of another dead
    //     cell can still call this function.
    GC_ALLOW_CELL_DESTRUCTOR virtual ~Function() override
    {
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

    template<typename Callback, typename Box>
    struct CaptureOperations {
        static constexpr Operations operations {
            {
                .signature = &Detail::function_signature<R(Args...)>,
                .visit_edges = [](void* box, Visitor& visitor) {
                    if (box)
                        static_cast<Box const*>(box)->visit_edges(visitor); },
                .destroy = [](void* box) {
                    if (box)
                        static_cast<Box*>(box)->~Box(); },
            },
            [](void* box, Args... args) -> R {
                if constexpr (IsSame<Box, Detail::CaptureBox<IndexSequence<>>>)
                    return Callback {}(forward<Args>(args)...);
                else
                    return static_cast<Box*>(box)->template invoke<Callback, R>(forward<Args>(args)...);
            },
        };
    };

    FunctionTableHandle m_handle;
};

// Makes a function of a callable that takes everything it uses from its captures. The values named by GC::captures()
// come first among the callable's parameters, followed by the function's own.
// NB: No template argument can be given for the leading pack, so a function type given explicitly selects the overload
//     below instead of being taken as the type of the first capture.
template<int&... ExplicitArgumentBarrier, typename... Values, typename Callable, typename T = typename Detail::DropParameters<sizeof...(Values), typename Detail::CallableTypeWithCaptures<RemoveCVReference<Callable>, typename Detail::CaptureStorage<Values>::Type...>::Type>::Type>
static Ref<Function<T>> create_function(Heap& heap, CaptureList<Values...>&& captures, Callable&& callable)
{
    return Function<T>::create(heap, move(captures), forward<Callable>(callable));
}

// The same, for a callable whose type does not name the function type, such as one that takes its own parameters as auto.
template<typename T, typename... Values, typename Callable>
requires(IsFunction<T>)
static Ref<Function<T>> create_function(Heap& heap, CaptureList<Values...>&& captures, Callable&& callable)
{
    return Function<T>::create(heap, move(captures), forward<Callable>(callable));
}

// Makes a function of a callable that captures nothing.
template<typename Callable, typename T = EquivalentFunctionType<RemoveCVReference<Callable>>>
static Ref<Function<T>> create_function(Heap& heap, Callable&& function)
{
    static_assert(Detail::CapturesNothing<RemoveCVReference<Callable>>, "A GC::Function callable captures nothing; name what it uses with GC::captures() and take it as parameters");
    return Function<T>::create(heap, CaptureList<> {}, forward<Callable>(function));
}

template<typename R, typename... Args>
GC_DEFINE_ALLOCATOR(Function<R(Args...)>);

}
