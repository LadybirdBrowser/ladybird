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
#include <AK/SourceLocation.h>
#include <AK/StringView.h>
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
        void visit(Cell* cell, SourceLocation location = SourceLocation::current())
        {
            if (cell)
                visit_impl(*cell, location);
        }

        void visit(Cell& cell)
        {
            visit_impl(cell, {});
        }

        void visit(Cell const* cell)
        {
            if (cell)
                visit_impl(const_cast<Cell&>(*cell), {});
        }

        void visit(Cell const& cell)
        {
            visit_impl(const_cast<Cell&>(cell), {});
        }

        template<typename T>
        void visit(Ptr<T> cell, SourceLocation location = SourceLocation::current())
        {
            if (cell)
                visit_impl(const_cast<RemoveConst<T>&>(*cell.ptr()), location);
        }

        template<typename T>
        void visit(Ref<T> cell, SourceLocation location = SourceLocation::current())
        {
            visit_impl(const_cast<RemoveConst<T>&>(*cell.ptr()), location);
        }

        template<typename T>
        void visit(ReadonlySpan<T> span, SourceLocation location = SourceLocation::current())
        {
            for (auto& value : span)
                visit(value, location);
        }

        template<typename T>
        void visit(ReadonlySpan<T> span, SourceLocation location = SourceLocation::current())
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            visit_impl(ReadonlySpan<NanBoxedValue>(span.data(), span.size()), location);
        }

        template<typename T>
        void visit(Span<T> span, SourceLocation location = SourceLocation::current())
        {
            for (auto& value : span)
                visit(value, location);
        }

        template<typename T>
        void visit(Span<T> span, SourceLocation location = SourceLocation::current())
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            visit_impl(ReadonlySpan<NanBoxedValue>(span.data(), span.size()), location);
        }

        template<typename T, size_t inline_capacity>
        void visit(Vector<T, inline_capacity> const& vector, SourceLocation location = SourceLocation::current())
        {
            for (auto& value : vector)
                visit(value, location);
        }

        template<typename T, size_t inline_capacity>
        void visit(Vector<T, inline_capacity> const& vector, SourceLocation location = SourceLocation::current())
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            visit_impl(ReadonlySpan<NanBoxedValue>(vector.span().data(), vector.size()), location);
        }

        template<typename T>
        void visit(HashTable<T> const& table, SourceLocation location = SourceLocation::current())
        {
            for (auto& value : table)
                visit(value, location);
        }

        template<typename T>
        void visit(OrderedHashTable<T> const& table, SourceLocation location = SourceLocation::current())
        {
            for (auto& value : table)
                visit(value, location);
        }

        template<typename K, typename V, typename T>
        void visit(HashMap<K, V, T> const& map, SourceLocation location = SourceLocation::current())
        {
            for (auto& it : map) {
                if constexpr (requires { visit(it.key); })
                    visit(it.key, location);
                if constexpr (requires { visit(it.value); })
                    visit(it.value, location);
            }
        }

        template<typename K, typename V, typename T>
        void visit(OrderedHashMap<K, V, T> const& map, SourceLocation location = SourceLocation::current())
        {
            for (auto& it : map) {
                if constexpr (requires { visit(it.key); })
                    visit(it.key, location);
                if constexpr (requires { visit(it.value); })
                    visit(it.value, location);
            }
        }

        template<typename T>
        void visit(Optional<T> const& optional, SourceLocation location = SourceLocation::current())
        {
            if (optional.has_value())
                visit(optional.value(), location);
        }

        void visit(NanBoxedValue const& value);

        template<typename T>
        void visit(T const& value, SourceLocation location = SourceLocation::current())
        requires(IsBaseOf<NanBoxedValue, T>)
        {
            if (value.is_cell())
                visit_impl(value.as_cell(), location);
        }

        // Allow explicitly ignoring a GC-allocated member in a visit_edges implementation instead
        // of just not using it.
        template<typename T>
        void ignore(T const&)
        {
        }

        virtual void visit_possible_values(ReadonlyBytes) = 0;

    protected:
        virtual void visit_impl(Cell&, SourceLocation) = 0;
        virtual void visit_impl(ReadonlySpan<NanBoxedValue>, SourceLocation) = 0;
        virtual ~Visitor() = default;
    };

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
};

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
