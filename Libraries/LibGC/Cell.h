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
#include <LibGC/CAPI.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>
#include <LibGC/Member.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>

namespace GC {

namespace Detail {

// Whether a value of the type holds a pointer to a cell, as far as Cell::Visitor::ignore() checks.
template<typename T>
struct HoldsCellPointer : FalseType { };
template<typename T>
struct HoldsCellPointer<T*> {
    static constexpr bool value = IsBaseOf<Cell, RemoveCV<T>> || IsBaseOf<ForeignCell, RemoveCV<T>>;
};
template<typename T>
struct HoldsCellPointer<Ptr<T>> : TrueType { };
template<typename T>
struct HoldsCellPointer<Ref<T>> : TrueType { };
template<typename T>
struct HoldsCellPointer<Member<T>> : TrueType { };
template<typename T>
struct HoldsCellPointer<NonnullMember<T>> : TrueType { };
template<typename T>
struct HoldsCellPointer<Optional<T>> : HoldsCellPointer<T> { };
template<typename T, size_t inline_capacity, FastLastAccess fast_last_access>
struct HoldsCellPointer<Vector<T, inline_capacity, fast_last_access>> : HoldsCellPointer<T> { };
template<typename T, typename Traits, bool IsOrdered>
struct HoldsCellPointer<HashTable<T, Traits, IsOrdered>> : HoldsCellPointer<T> { };
template<typename K, typename V, typename KeyTraits, typename ValueTraits, bool IsOrdered>
struct HoldsCellPointer<HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>> {
    static constexpr bool value = HoldsCellPointer<K>::value || HoldsCellPointer<V>::value;
};

}

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

    // Whether the last collection marked the cell, which it keeps in the descriptor of the cell's block. Only meaningful
    // from the end of marking until the cell's block is swept.
    bool is_marked() const;

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
                visit_cell(*cell);
        }

        void visit(Cell& cell)
        {
            visit_cell(cell);
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
                visit_cell(*as_cell(const_cast<RemoveConst<T>*>(cell.ptr())));
        }

        template<typename T>
        void visit(Ref<T> cell)
        {
            visit_cell(*as_cell(const_cast<RemoveConst<T>*>(cell.ptr())));
        }

        // Members are visited through their slot, which a collection that moves cells rewrites.
        template<typename T>
        void visit(Member<T> const& slot)
        {
            static_assert(sizeof(slot) == sizeof(u32));
            if (m_marker) {
                gc_marker_visit_member(m_marker, reinterpret_cast<u32 const*>(&slot));
                return;
            }
            if (slot)
                visit_member_cell(slot.ptr());
        }

        template<typename T>
        void visit(NonnullMember<T> const& slot)
        {
            static_assert(sizeof(slot) == sizeof(u32));
            if (m_marker) {
                gc_marker_visit_member(m_marker, reinterpret_cast<u32 const*>(&slot));
                return;
            }
            visit_member_cell(slot.ptr());
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
            visit_values(ReadonlySpan<NanBoxedValue>(span.data(), span.size()));
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
            visit_values(ReadonlySpan<NanBoxedValue>(span.data(), span.size()));
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
            visit_values(ReadonlySpan<NanBoxedValue>(vector.span().data(), vector.size()));
        }

        template<typename T, size_t size>
        void visit(Array<T, size> const& array)
        requires(IsVisitable<T>::value)
        {
            visit(array.span());
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

        // Allow explicitly ignoring a member in a visit_edges implementation instead of just not using it. A pointer to a
        // cell that nothing visits goes stale once the cell moves, so a weak reference to a cell has to be a GC::Weak.
        template<typename T>
        void ignore(T const&)
        requires(!Detail::HoldsCellPointer<T>::value)
        {
        }

        // Visits a value that is reachable only while key is, such as a WeakMap entry's. Only marking tells them apart.
        // The caller must drop its entries whose key died, or the next collection reads a freed key.
        virtual void visit_ephemeron(Cell& key, NanBoxedValue const& value);

        static size_t marker_offset();

    protected:
        template<typename T>
        ALWAYS_INLINE void visit_member_cell(T* cell)
        {
            auto* mutable_cell = as_cell(const_cast<RemoveConst<T>*>(cell));
            // A compressed pointer must name the start of its cell.
            ASSERT(static_cast<void const*>(mutable_cell) == static_cast<void const*>(cell));
            visit_cell(*mutable_cell);
        }

        // NB: A marking visitor hands edges to its marker directly, rather than through its own visit_impl().
        ALWAYS_INLINE void visit_cell(Cell& cell)
        {
            if (m_marker) {
                gc_marker_visit_cell(m_marker, reinterpret_cast<GCCell*>(&cell));
                return;
            }
            visit_impl(cell);
        }

        ALWAYS_INLINE void visit_values(ReadonlySpan<NanBoxedValue> values)
        {
            if (m_marker) {
                if (!values.is_empty())
                    gc_marker_visit_values(m_marker, reinterpret_cast<uint64_t const*>(values.data()), values.size());
                return;
            }
            visit_impl(values);
        }

        // The marker of the collection that a marking visitor marks for, and null in every other visitor. Foreign
        // implementations find it at GCLayout::visitor_marker_offset.
        GCMarker* m_marker { nullptr };

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

    // NB: Holds the state of the cell's identity hash, see identity_hash().
    [[maybe_unused]] u8 m_identity_hash_state { 0 };
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
    bool is_marked() const { return as_cell(this)->is_marked(); }
    Cell::State state() const { return m_state; }

    ALWAYS_INLINE Heap& heap() const { return HeapBlockBase::from_cell(as_cell(this))->heap(); }
    ALWAYS_INLINE CellTypeInfo const& type_info() const { return HeapBlockBase::from_cell(as_cell(this))->type_info(); }

private:
    friend struct CAPI;

    [[maybe_unused]] void const* m_word_owned_by_the_foreign_implementation;
    // NB: As in Cell.
    [[maybe_unused]] u8 m_identity_hash_state;
    Cell::State m_state;
    CellKind m_cell_kind;
    // The foreign implementation lays its fields out right after the header. Naming these bytes keeps GCC from
    // placing a field of a derived type in the tail padding, where it would overlap them without changing the size.
    [[maybe_unused]] u8 m_tail_owned_by_the_foreign_implementation[sizeof(void*) - sizeof(u8) - sizeof(Cell::State) - sizeof(CellKind)];
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

// Where a cell keeps the state of its identity hash, and the bits of that state. CAPI.cpp checks them against the cell
// header and the Rust side.
namespace IdentityHashState {

static constexpr size_t offset = 8;
static constexpr u8 hashed = 1 << 0;
static constexpr u8 moved = 1 << 1;

}

// The hash of a cell's identity, which stays the same while the cell lives, wherever the collector moves it. Tables that
// are keyed by cells that may move hash them with this. See gc_cell_identity_hash().
ALWAYS_INLINE u32 identity_hash(void const* cell)
{
    // NB: The fast path of gc_cell_identity_hash(), for cells that did not move since they were first hashed.
    auto* state = static_cast<u8*>(const_cast<void*>(cell)) + IdentityHashState::offset;
    if (!(*state & IdentityHashState::moved)) [[likely]] {
        *state |= IdentityHashState::hashed;
        return compress(cell);
    }
    return gc_cell_identity_hash(static_cast<GCCell const*>(cell));
}

// The hash of a cell that a table which holds it weakly keys by, or of null once the cell is gone. A cell defined
// outside of C++ may move, which keeps its identity hash but not its address, see WeakBlock::update_moved_cells().
template<typename T>
ALWAYS_INLINE unsigned weak_key_hash(T const* cell)
{
    if constexpr (IsBaseOf<ForeignCell, T>)
        return cell ? u32_hash(identity_hash(cell)) : 0;
    else
        return Traits<T const*>::hash(cell);
}

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
