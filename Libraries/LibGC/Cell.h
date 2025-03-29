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
#include <AK/StringView.h>
#include <AK/Weakable.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>
#include <LibGC/Ptr.h>

namespace GC {

// This instrumentation tells analysis tooling to ignore a potentially mis-wrapped GC-allocated member variable
// It should only be used when the lifetime of the GC-allocated member is always longer than the object
#if defined(AK_COMPILER_CLANG)
#    define IGNORE_GC [[clang::annotate("serenity::ignore_gc")]]
#else
#    define IGNORE_GC
#endif

#define GC_CELL(class_, base_class)                \
public:                                            \
    using Base = base_class;                       \
    virtual StringView class_name() const override \
    {                                              \
        return #class_##sv;                        \
    }                                              \
    friend class GC::Heap;

class Cell {
    AK_MAKE_NONCOPYABLE(Cell);
    AK_MAKE_NONMOVABLE(Cell);

public:
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

    class Visitor {
    public:
        void visit(Cell* cell)
        {
            if (cell)
                visit_impl(*cell);
        }

        void visit(Cell& cell)
        {
            visit_impl(cell);
        }

        void visit(Cell const* cell)
        {
            visit(const_cast<Cell*>(cell));
        }

        void visit(Cell const& cell)
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
        void visit(Span<T> span)
        {
            for (auto& value : span)
                visit(value);
        }

        template<typename T>
        void visit(Vector<T> const& vector)
        {
            for (auto& value : vector)
                visit(value);
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

        void visit(NanBoxedValue const& value);

        // Allow explicitly ignoring a GC-allocated member in a visit_edges implementation instead
        // of just not using it.
        template<typename T>
        void ignore(T const&)
        {
        }

        virtual void visit_possible_values(ReadonlyBytes) = 0;

    protected:
        virtual void visit_impl(Cell&) = 0;
        virtual ~Visitor() = default;
    };

    virtual void visit_edges(Visitor&) { }

    // This will be called on unmarked objects by the garbage collector in a separate pass before destruction.
    virtual void finalize() { }

    // This allows cells to survive GC by choice, even if nothing points to them.
    // It's used to implement special rules in the web platform.
    // NOTE: Cells must call set_overrides_must_survive_garbage_collection() for this to be honored.
    virtual bool must_survive_garbage_collection() const { return false; }

    bool overrides_must_survive_garbage_collection(Badge<Heap>) const { return m_overrides_must_survive_garbage_collection; }

    ALWAYS_INLINE Heap& heap() const { return HeapBlockBase::from_cell(this)->heap(); }

protected:
    Cell() = default;

    ALWAYS_INLINE void* private_data() const { return bit_cast<HeapBase*>(&heap())->private_data(); }

    void set_overrides_must_survive_garbage_collection(bool b) { m_overrides_must_survive_garbage_collection = b; }

private:
    bool m_mark : 1 { false };
    bool m_overrides_must_survive_garbage_collection : 1 { false };
    State m_state : 1 { State::Live };
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
