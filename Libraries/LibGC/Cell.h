/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Swift.h>
#include <AK/Weakable.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>

namespace GC {

// This instrumentation tells analysis tooling to ignore a potentially mis-wrapped GC-allocated member variable
// It should only be used when the lifetime of the GC-allocated member is always longer than the object
#if defined(AK_COMPILER_CLANG)
#    define IGNORE_GC [[clang::annotate("serenity::ignore_gc")]]
#    define GC_ALLOW_CELL_DESTRUCTOR [[clang::annotate("ladybird::allow_cell_destructor")]]
#else
#    define IGNORE_GC
#    define GC_ALLOW_CELL_DESTRUCTOR
#endif

#define GC_CELL(class_, base_class)                \
public:                                            \
    using Base = base_class;                       \
    virtual StringView class_name() const override \
    {                                              \
        return #class_##sv;                        \
    }                                              \
    friend class GC::Heap;

class GC_API Cell {
    AK_MAKE_NONCOPYABLE(Cell);
    AK_MAKE_NONMOVABLE(Cell);

public:
    static constexpr bool OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION = false;
    static constexpr bool OVERRIDES_FINALIZE = false;

    virtual ~Cell() = default;

    bool is_marked() const { return m_mark; }
    void set_marked(bool b) { m_mark = b; }

    enum class State : bool {
        Live,
        Dead,
    };

    State state() const { return m_state; }
    void set_state(State state) { m_state = state; }

    virtual StringView class_name() const = 0;

    class GC_API Visitor {
    public:
        void visit(Cell* cell)
        {
            if (cell)
                visit_impl(*cell);
        }

        void visit(Cell& cell) SWIFT_NAME(visitRef(_:))
        {
            visit_impl(cell);
        }

        void visit(Cell const* cell) SWIFT_NAME(visitConst(_:))
        {
            visit(const_cast<Cell*>(cell));
        }

        void visit(Cell const& cell) SWIFT_NAME(visitConstRef(_:))
        {
            visit(const_cast<Cell&>(cell));
        }

        template<typename T>
        void visit(Ptr<T> cell)
        {
            if (cell)
                visit_impl(const_cast<RemoveConst<T>&>(*cell.ptr()));
        }

        template<typename T>
        void visit(Ref<T> cell)
        {
            visit_impl(const_cast<RemoveConst<T>&>(*cell.ptr()));
        }

        template<typename T>
        void visit(ReadonlySpan<T> span)
        {
            for (auto& value : span)
                visit(value);
        }

        template<typename T>
        void visit(ReadonlySpan<T> span)
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            visit_impl(ReadonlySpan<NanBoxedValue>(span.data(), span.size()));
        }

        template<typename T>
        void visit(Span<T> span)
        {
            for (auto& value : span)
                visit(value);
        }

        template<typename T>
        void visit(Span<T> span)
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            visit_impl(ReadonlySpan<NanBoxedValue>(span.data(), span.size()));
        }

        template<typename T, size_t inline_capacity>
        void visit(Vector<T, inline_capacity> const& vector)
        {
            for (auto& value : vector)
                visit(value);
        }

        template<typename T, size_t inline_capacity>
        void visit(Vector<T, inline_capacity> const& vector)
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            visit_impl(ReadonlySpan<NanBoxedValue>(vector.span().data(), vector.size()));
        }

        template<typename T>
        void visit(HashTable<T> const& table)
        {
            for (auto& value : table)
                visit(value);
        }

        template<typename T>
        void visit(OrderedHashTable<T> const& table)
        {
            for (auto& value : table)
                visit(value);
        }

        template<typename K, typename V, typename T>
        void visit(HashMap<K, V, T> const& map)
        {
            for (auto& it : map) {
                if constexpr (requires { visit(it.key); })
                    visit(it.key);
                if constexpr (requires { visit(it.value); })
                    visit(it.value);
            }
        }

        template<typename K, typename V, typename T>
        void visit(OrderedHashMap<K, V, T> const& map)
        {
            for (auto& it : map) {
                if constexpr (requires { visit(it.key); })
                    visit(it.key);
                if constexpr (requires { visit(it.value); })
                    visit(it.value);
            }
        }

        template<typename T>
        void visit(Optional<T> const& optional)
        {
            if (optional.has_value())
                visit(optional.value());
        }

        void visit(NanBoxedValue const& value) SWIFT_NAME(visitValue(_:));

        // Allow explicitly ignoring a GC-allocated member in a visit_edges implementation instead
        // of just not using it.
        template<typename T>
        void ignore(T const&)
        {
        }

        virtual void visit_possible_values(ReadonlyBytes) = 0;

    protected:
        virtual void visit_impl(Cell&) = 0;
        virtual void visit_impl(ReadonlySpan<NanBoxedValue>) = 0;
        virtual ~Visitor() = default;
    } SWIFT_UNSAFE_REFERENCE;

    MUST_UPCALL virtual void visit_edges(Visitor&) { }

    // This will be called on unmarked objects by the garbage collector in a separate pass before destruction.
    MUST_UPCALL virtual void finalize() { }

    // This allows cells to survive GC by choice, even if nothing points to them.
    // It's used to implement special rules in the web platform.
    // NOTE: Cell types must have OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION set for this to be called.
    virtual bool must_survive_garbage_collection() const { return false; }

    ALWAYS_INLINE Heap& heap() const { return HeapBlockBase::from_cell(this)->heap(); }

protected:
    Cell() = default;

private:
    bool m_mark { false };
    State m_state { State::Live };
} SWIFT_UNSAFE_REFERENCE;

}

template<>
struct AK::Formatter<GC::Cell> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Cell const* cell)
    {
        if (!cell)
            return builder.put_string("Cell{nullptr}"sv);
        return Formatter<FormatString>::format(builder, "{}({})"sv, cell->class_name(), cell);
    }
};
