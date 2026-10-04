/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>

namespace GC {

template<typename T>
struct IsVisitable;

#if defined(AK_COMPILER_CLANG)
#    define GC_ALLOW_CELL_DESTRUCTOR [[clang::annotate("ladybird::allow_cell_destructor")]]
#    define GC_VISITS_THROUGH_FOREIGN_IMPLEMENTATION [[clang::annotate("ladybird::visits_through_foreign_implementation")]]
#else
#    define GC_ALLOW_CELL_DESTRUCTOR
#    define GC_VISITS_THROUGH_FOREIGN_IMPLEMENTATION
#endif

#define GC_CELL(class_, base_class)                \
public:                                            \
    using Base = base_class;                       \
    virtual StringView class_name() const override \
    {                                              \
        return #class_##sv;                        \
    }                                              \
    friend class GC::Heap;                         \
    friend struct GC::CellTypeThunks;

#define GC_CELL_WITH_CUSTOM_CLASS_NAME(class_, base_class) \
public:                                                    \
    using Base = base_class;                               \
    friend class GC::Heap;                                 \
    friend struct GC::CellTypeThunks;

// A coarse class tag stored in every cell header. It lets a holder of a cell pointer confirm what
// the cell really is with a single byte compare, without reading a vtable. Only the classes a
// JS::Value can point at need their own kind; everything else stays Other.
enum class CellKind : u8 {
    Other,
    Object,
    PrimitiveString,
    Symbol,
    BigInt,
    Accessor,
};

class GC_API Cell {
    AK_MAKE_NONCOPYABLE(Cell);
    AK_MAKE_NONMOVABLE(Cell);

public:
    // Heap::allocate() copies this into the header of every cell it creates. A class that a
    // JS::Value can point at overrides it, and its subclasses inherit the override.
    static constexpr CellKind cell_kind_for_class = CellKind::Other;

    virtual ~Cell() = default;

    CellKind cell_kind() const { return m_cell_kind; }

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

        void visit(ForeignCell* cell);
        void visit(ForeignCell& cell);
        void visit(ForeignCell const* cell);
        void visit(ForeignCell const& cell);

        template<typename T>
        void visit(Ptr<T> cell)
        {
            if (cell)
                visit_impl(*as_cell(const_cast<RemoveConst<T>*>(cell.ptr())));
        }

        template<typename T>
        void visit(Ref<T> cell)
        {
            visit_impl(*as_cell(const_cast<RemoveConst<T>*>(cell.ptr())));
        }

        template<typename T>
        void visit(ReadonlySpan<T> span)
        requires(!IsBaseOf<NanBoxedValue, T> && IsVisitable<T>::value)
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
        requires(!IsBaseOf<NanBoxedValue, T> && IsVisitable<T>::value)
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
        requires(!IsBaseOf<NanBoxedValue, T> && IsVisitable<T>::value)
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
        requires(IsVisitable<T>::value)
        {
            for (auto& value : table)
                visit(value);
        }

        template<typename T>
        void visit(OrderedHashTable<T> const& table)
        requires(IsVisitable<T>::value)
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
        void visit(T const& value)
        requires(!IsCellLike<T> && requires(T& visitable) { visitable.visit_edges(*this); })
        {
            const_cast<T&>(value).visit_edges(*this);
        }

        template<typename T>
        void visit(AK::RefPtr<T> const& value)
        requires(requires(RemoveConst<T>& visitable) { visitable.visit_edges(*this); })
        {
            if (value)
                visit(*value);
        }

        template<typename T>
        void visit(AK::NonnullRefPtr<T> const& value)
        requires(requires(RemoveConst<T>& visitable) { visitable.visit_edges(*this); })
        {
            visit(*value);
        }

        template<typename T>
        void visit(Optional<T> const& optional)
        requires(IsVisitable<T>::value)
        {
            if (optional.has_value())
                visit(optional.value());
        }

        void visit(NanBoxedValue const& value);

        template<typename... Ts>
        void visit(Variant<Ts...> const& variant)
        requires((IsVisitable<Ts>::value || ...))
        {
            variant.visit([&](auto const& value) {
                if constexpr (requires { visit(value); })
                    visit(value);
            });
        }

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
    };

    MUST_UPCALL virtual void visit_edges(Visitor&) { }

    // This will be called on unmarked objects by the garbage collector in a separate pass before destruction.
    MUST_UPCALL virtual void finalize() { }

    virtual size_t external_memory_size() const { return 0; }

    ALWAYS_INLINE Heap& heap() const { return HeapBlockBase::from_cell(this)->heap(); }
    ALWAYS_INLINE CellTypeInfo const& type_info() const { return HeapBlockBase::from_cell(this)->type_info(); }

protected:
    Cell() = default;

private:
    friend class Heap;
    friend struct CAPI;

    void set_cell_kind(CellKind kind) { m_cell_kind = kind; }

    bool m_mark { false };
    State m_state { State::Live };
    CellKind m_cell_kind { CellKind::Other };
};

// A cell whose type is implemented outside of C++ and allocated through LibGC/CAPI.h, such as a cell of the Rust
// LibJS runtime. Its header is laid out like a Cell's, except that the first word belongs to the foreign
// implementation instead of holding a vtable pointer. C++ code names these cells through types that derive from
// ForeignCell and add neither fields nor virtual functions, so that a pointer to such a type is the foreign cell
// itself. LibGC dispatches through the type info of a cell's block, never through a vtable, so Ptr, Ref, Root, Weak
// and visitors treat these types like subclasses of Cell. Only the foreign implementation creates and destroys them.
class ForeignCell {
    AK_MAKE_NONCOPYABLE(ForeignCell);
    AK_MAKE_NONMOVABLE(ForeignCell);

public:
    ForeignCell() = delete;
    ~ForeignCell() = delete;

    CellKind cell_kind() const { return m_cell_kind; }
    bool is_marked() const { return m_mark; }
    Cell::State state() const { return m_state; }

    ALWAYS_INLINE Heap& heap() const { return HeapBlockBase::from_cell(as_cell(this))->heap(); }
    ALWAYS_INLINE CellTypeInfo const& type_info() const { return HeapBlockBase::from_cell(as_cell(this))->type_info(); }

private:
    friend struct CAPI;

    [[maybe_unused]] void const* m_word_owned_by_the_foreign_implementation;
    bool m_mark;
    Cell::State m_state;
    CellKind m_cell_kind;
    // The foreign implementation lays its fields out right after the header. Naming these bytes keeps GCC from
    // placing a field of a derived type in the tail padding, where it would overlap them without changing the size.
    [[maybe_unused]] u8 m_tail_owned_by_the_foreign_implementation[sizeof(void*) - sizeof(bool) - sizeof(Cell::State) - sizeof(CellKind)];
};

inline void Cell::Visitor::visit(ForeignCell* cell)
{
    if (cell)
        visit_impl(*as_cell(cell));
}

inline void Cell::Visitor::visit(ForeignCell& cell)
{
    visit_impl(*as_cell(&cell));
}

inline void Cell::Visitor::visit(ForeignCell const* cell)
{
    visit(const_cast<ForeignCell*>(cell));
}

inline void Cell::Visitor::visit(ForeignCell const& cell)
{
    visit(const_cast<ForeignCell&>(cell));
}

template<typename T>
struct IsVisitable {
    static constexpr bool value = requires(Cell::Visitor& visitor, T const& value) { visitor.visit(value); };
};

GC_API StringView class_name_of(Cell const&);

inline StringView class_name_of(ForeignCell const& cell)
{
    return class_name_of(*as_cell(&cell));
}

}

template<>
struct AK::Formatter<GC::Cell> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Cell const& cell)
    {
        return Formatter<FormatString>::format(builder, "{}({})"sv, GC::class_name_of(cell), &cell);
    }
};

template<>
struct AK::Formatter<GC::ForeignCell> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, GC::ForeignCell const& cell)
    {
        return Formatter<FormatString>::format(builder, "{}({})"sv, GC::class_name_of(cell), &cell);
    }
};
