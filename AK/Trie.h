/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>

namespace AK {

namespace Detail {

template<typename TypeA, typename Default>
struct SubstituteIfVoid {
    using Type = TypeA;
};

template<typename Default>
struct SubstituteIfVoid<void, Default> {
    using Type = Default;
};

template<typename DeclaredBaseType, typename DefaultBaseType, typename ValueType, typename MetadataT, typename ValueTraits, template<typename, typename, typename> typename MapType>
class Trie {
    using BaseType = typename SubstituteIfVoid<DeclaredBaseType, DefaultBaseType>::Type;

public:
    using MetadataType = MetadataT;

    Trie(ValueType value, Optional<MetadataType> metadata)
        : m_value(move(value))
        , m_metadata(move(metadata))
    {
    }

    template<typename It>
    BaseType& traverse_until_last_accessible_node(It& it, It const& end)
    {
        Trie* node = this;
        for (; it < end; ++it) {
            auto next_it = node->m_children.find(*it);
            if (next_it == node->m_children.end())
                return static_cast<BaseType&>(*node);
            node = &*(*next_it).value;
        }
        return static_cast<BaseType&>(*node);
    }

    template<typename It>
    BaseType const& traverse_until_last_accessible_node(It& it, It const& end) const { return const_cast<Trie*>(this)->traverse_until_last_accessible_node(it, end); }

    template<typename It>
    BaseType& traverse_until_last_accessible_node(It const& begin, It const& end)
    {
        auto it = begin;
        return const_cast<Trie*>(this)->traverse_until_last_accessible_node(it, end);
    }

    template<typename It>
    BaseType const& traverse_until_last_accessible_node(It const& begin, It const& end) const
    {
        auto it = begin;
        return const_cast<Trie*>(this)->traverse_until_last_accessible_node(it, end);
    }

    bool has_metadata() const { return m_metadata.has_value(); }

    Optional<MetadataType> metadata() const
    requires(!IsNullPointer<MetadataType>)
    {
        return m_metadata;
    }
    void set_metadata(MetadataType metadata)
    requires(!IsNullPointer<MetadataType>)
    {
        m_metadata = move(metadata);
    }
    MetadataType const& metadata_value() const
    requires(!IsNullPointer<MetadataType>)
    {
        return m_metadata.value();
    }
    MetadataType& metadata_value()
    requires(!IsNullPointer<MetadataType>)
    {
        return m_metadata.value();
    }

    ValueType const& value() const { return m_value; }
    ValueType& value() { return m_value; }

    ErrorOr<Trie*> ensure_child(ValueType value, Optional<MetadataType> metadata = {})
    {
        auto it = m_children.find(value);
        if (it == m_children.end()) {
            OwnPtr<Trie> node;
            if constexpr (requires { { value->try_clone() } -> SpecializationOf<ErrorOr>; })
                node = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Trie(TRY(value->try_clone()), move(metadata))));
            else
                node = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Trie(value, move(metadata))));
            auto& node_ref = *node;
            TRY(m_children.try_set(move(value), node.release_nonnull()));
            return &static_cast<BaseType&>(node_ref);
        }

        auto& node_ref = *it->value;
        if (metadata.has_value())
            node_ref.m_metadata = move(metadata);
        return &static_cast<BaseType&>(node_ref);
    }

    template<typename It, typename ProvideMetadataFunction>
    ErrorOr<BaseType*> insert(
        It& it, It const& end, MetadataType metadata, ProvideMetadataFunction provide_missing_metadata)
    requires(!IsNullPointer<MetadataType>)
    {
        Trie* last_root_node = &traverse_until_last_accessible_node(it, end);
        auto invoke_provide_missing_metadata = [&]<typename... Ts>(Ts&&... args) -> ErrorOr<Optional<MetadataType>> {
            if constexpr (SameAs<MetadataType, decltype(provide_missing_metadata(forward<Ts>(args)...))>)
                return Optional<MetadataType>(provide_missing_metadata(forward<Ts>(args)...));
            else
                return provide_missing_metadata(forward<Ts>(args)...);
        };
        for (; it != end; ++it) {
            if constexpr (requires { { ValueType::ElementType::try_create(*it) } -> SpecializationOf<ErrorOr>; })
                last_root_node = static_cast<Trie*>(TRY(last_root_node->ensure_child(TRY(ValueType::ElementType::try_create(*it)), TRY(invoke_provide_missing_metadata(static_cast<BaseType&>(*last_root_node), it)))));
            else
                last_root_node = static_cast<Trie*>(TRY(last_root_node->ensure_child(*it, TRY(invoke_provide_missing_metadata(static_cast<BaseType&>(*last_root_node), it)))));
        }
        last_root_node->set_metadata(move(metadata));
        return static_cast<BaseType*>(last_root_node);
    }

    template<typename It>
    ErrorOr<BaseType*> insert(It& it, It const& end)
    requires(IsNullPointer<MetadataType>)
    {
        Trie* last_root_node = &traverse_until_last_accessible_node(it, end);
        for (; it != end; ++it) {
            if constexpr (requires { { ValueType::ElementType::try_create(*it) } -> SpecializationOf<ErrorOr>; })
                last_root_node = static_cast<Trie*>(TRY(last_root_node->ensure_child(TRY(ValueType::ElementType::try_create(*it)), {})));
            else
                last_root_node = static_cast<Trie*>(TRY(last_root_node->ensure_child(*it, {})));
        }
        return static_cast<BaseType*>(last_root_node);
    }

    template<typename It, typename ProvideMetadataFunction>
    ErrorOr<BaseType*> insert(
        It const& begin, It const& end, MetadataType metadata, ProvideMetadataFunction provide_missing_metadata)
    requires(!IsNullPointer<MetadataType>)
    {
        auto it = begin;
        return insert(it, end, move(metadata), move(provide_missing_metadata));
    }

    template<typename It>
    ErrorOr<BaseType*> insert(It const& begin, It const& end)
    requires(IsNullPointer<MetadataType>)
    {
        auto it = begin;
        return insert(it, end);
    }

    MapType<ValueType, NonnullOwnPtr<Trie>, ValueTraits>& children() { return m_children; }
    MapType<ValueType, NonnullOwnPtr<Trie>, ValueTraits> const& children() const { return m_children; }

    template<typename Fn>
    ErrorOr<void> for_each_node_in_tree_order(Fn callback) const
    {
        struct State {
            bool did_generate_root { false };
            typename MapType<ValueType, NonnullOwnPtr<Trie>, ValueTraits>::ConstIteratorType it;
            typename MapType<ValueType, NonnullOwnPtr<Trie>, ValueTraits>::ConstIteratorType end;
        };
        Vector<State> state;
        TRY(state.try_empend(false, m_children.begin(), m_children.end()));

        auto invoke = [&](auto& current_node) -> ErrorOr<IterationDecision> {
            if constexpr (VoidFunction<Fn, BaseType const&>) {
                callback(static_cast<BaseType const&>(current_node));
                return IterationDecision::Continue;
            } else if constexpr (IsSpecializationOf<decltype(callback(declval<BaseType const&>())), ErrorOr>) {
                return callback(static_cast<BaseType const&>(current_node));
            } else if constexpr (IteratorFunction<Fn, BaseType const&>) {
                return callback(static_cast<BaseType const&>(current_node));
            } else {
                static_assert(DependentFalse<Fn>, "Invalid iterator function type signature");
            }
            return IterationDecision::Continue;
        };

        for (auto* current_node = this; current_node != nullptr;) {
            if (TRY(invoke(*current_node)) == IterationDecision::Break)
                break;
            TRY(skip_to_next_iterator(state, current_node));
        }
        return {};
    }

    [[nodiscard]] bool is_empty() const { return m_children.is_empty(); }
    void clear() { m_children.clear(); }

    ErrorOr<BaseType> deep_copy()
    requires(requires(ValueType value) { { value->try_clone() } -> SpecializationOf<ErrorOr>; })
    {
        Trie root(TRY(m_value->try_clone()), TRY(copy_metadata(m_metadata)));
        for (auto& it : m_children)
            TRY(root.m_children.try_set(TRY(it.key->try_clone()), TRY(adopt_nonnull_own_or_enomem(new (nothrow) Trie(TRY(it.value->deep_copy()))))));
        return static_cast<BaseType&&>(move(root));
    }

    ErrorOr<BaseType> deep_copy()
    {
        Trie root(m_value, TRY(copy_metadata(m_metadata)));
        for (auto& it : m_children)
            TRY(root.m_children.try_set(it.key, TRY(adopt_nonnull_own_or_enomem(new (nothrow) Trie(TRY(it.value->deep_copy()))))));
        return static_cast<BaseType&&>(move(root));
    }

private:
    static ErrorOr<Optional<MetadataType>> copy_metadata(Optional<MetadataType> const& metadata)
    {
        if (!metadata.has_value())
            return Optional<MetadataType> {};

        if constexpr (requires(MetadataType t) { { t.copy() } -> SpecializationOf<ErrorOr>; })
            return Optional<MetadataType> { TRY(metadata->copy()) };
        else
            return Optional<MetadataType> { MetadataType(metadata.value()) };
    }

    static ErrorOr<void> skip_to_next_iterator(auto& state, auto& current_node)
    {
        auto& current_state = state.last();
        if (current_state.did_generate_root)
            ++current_state.it;
        else
            current_state.did_generate_root = true;

        if (current_state.it == current_state.end)
            return pop_and_get_next_iterator(state, current_node);

        current_node = &*(*current_state.it).value;

        TRY(state.try_empend(false, current_node->m_children.begin(), current_node->m_children.end()));
        return {};
    }

    static ErrorOr<void> pop_and_get_next_iterator(auto& state, auto& current_node)
    {
        state.take_last();
        if (state.is_empty()) {
            current_node = nullptr;
            return {};
        }
        return skip_to_next_iterator(state, current_node);
    }

    ValueType m_value;
    Optional<MetadataType> m_metadata;
    MapType<ValueType, NonnullOwnPtr<Trie>, ValueTraits> m_children;
};

template<typename BaseType, typename DefaultBaseType, typename ValueType, typename ValueTraits, template<typename, typename, typename> typename MapType>
class Trie<BaseType, DefaultBaseType, ValueType, void, ValueTraits, MapType> : public Trie<BaseType, DefaultBaseType, ValueType, decltype(nullptr), ValueTraits, MapType> {
    using Trie<BaseType, DefaultBaseType, ValueType, decltype(nullptr), ValueTraits, MapType>::Trie;
};

template<typename K, typename V, typename T>
using HashMapForTrie = HashMap<K, V, T>;

}

template<typename ValueType, typename MetadataT = void, typename ValueTraits = Traits<ValueType>, typename BaseT = void, template<typename, typename, typename> typename MapType = Detail::HashMapForTrie>
class Trie : public Detail::Trie<BaseT, Trie<ValueType, MetadataT, ValueTraits, void, MapType>, ValueType, MetadataT, ValueTraits, MapType> {
public:
    using DetailTrie = Detail::Trie<BaseT, Trie<ValueType, MetadataT, ValueTraits, void, MapType>, ValueType, MetadataT, ValueTraits, MapType>;
    using MetadataType = typename DetailTrie::MetadataType;

    Trie(ValueType value, MetadataType metadata)
    requires(!IsVoid<MetadataType> && !IsNullPointer<MetadataType>)
        : DetailTrie(move(value), move(metadata))
    {
    }

    explicit Trie(ValueType value)
        : DetailTrie(move(value), Optional<MetadataType> {})
    {
    }
};

}

#if USING_AK_GLOBALLY
using AK::Trie;
#endif
