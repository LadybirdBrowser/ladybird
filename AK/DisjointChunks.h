/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AllOf.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Try.h>

namespace AK {

template<typename ChunkType, bool IsConst, size_t InlineCapacity = 0>
struct DisjointIterator {
    struct EndTag {
    };
    using ReferenceType = Conditional<IsConst, AddConst<Vector<ChunkType, InlineCapacity>>, Vector<ChunkType, InlineCapacity>>&;

    DisjointIterator(ReferenceType chunks)
        : m_chunks(chunks)
    {
        while (m_chunk_index < m_chunks.size() && m_chunks[m_chunk_index].is_empty())
            ++m_chunk_index;
    }

    DisjointIterator(ReferenceType chunks, EndTag)
        : m_chunk_index(chunks.size())
        , m_index_in_chunk(0)
        , m_chunks(chunks)
    {
    }

    DisjointIterator& operator++()
    {
        if (m_chunk_index >= m_chunks.size())
            return *this;

        auto& chunk = m_chunks[m_chunk_index];
        if (m_index_in_chunk + 1 >= chunk.size()) {
            ++m_chunk_index;
            m_index_in_chunk = 0;
        } else {
            ++m_index_in_chunk;
        }
        if (m_chunk_index < m_chunks.size()) {
            while (m_chunks[m_chunk_index].is_empty())
                ++m_chunk_index;
        }
        return *this;
    }

    bool operator==(DisjointIterator const& other) const
    {
        return &other.m_chunks == &m_chunks && other.m_index_in_chunk == m_index_in_chunk && other.m_chunk_index == m_chunk_index;
    }

    auto& operator*()
    requires(!IsConst)
    {
        return m_chunks[m_chunk_index][m_index_in_chunk];
    }
    auto* operator->()
    requires(!IsConst)
    {
        return &m_chunks[m_chunk_index][m_index_in_chunk];
    }
    auto const& operator*() const { return m_chunks[m_chunk_index][m_index_in_chunk]; }
    auto const* operator->() const { return &m_chunks[m_chunk_index][m_index_in_chunk]; }

private:
    size_t m_chunk_index { 0 };
    size_t m_index_in_chunk { 0 };
    ReferenceType m_chunks;
};

template<typename T, typename SpanContainer = Vector<Span<T>>>
class DisjointSpans {
public:
    DisjointSpans() = default;
    ~DisjointSpans() = default;
    DisjointSpans(DisjointSpans const&) = default;
    DisjointSpans(DisjointSpans&&) = default;

    explicit DisjointSpans(SpanContainer spans)
        : m_spans(move(spans))
    {
    }

    DisjointSpans& operator=(DisjointSpans&&) = default;
    DisjointSpans& operator=(DisjointSpans const&) = default;

    Span<T> singular_span() const
    {
        VERIFY(m_spans.size() == 1);
        return m_spans[0];
    }

    SpanContainer const& individual_spans() const { return m_spans; }

    bool operator==(DisjointSpans const& other) const
    {
        if (other.size() != size())
            return false;

        auto it = begin();
        auto other_it = other.begin();
        for (; it != end(); ++it, ++other_it) {
            if (*it != *other_it)
                return false;
        }
        return true;
    }

    T& operator[](size_t index) { return at(index); }
    T const& operator[](size_t index) const { return at(index); }
    T const& at(size_t index) const { return const_cast<DisjointSpans&>(*this).at(index); }
    T& at(size_t index)
    {
        auto value = find(index);
        VERIFY(value != nullptr);
        return *value;
    }
    T* find(size_t index)
    {
        auto span_and_offset = span_around(index);
        if (span_and_offset.offset >= span_and_offset.span.size())
            return nullptr;
        return &span_and_offset.span.at(span_and_offset.offset);
    }
    T const* find(size_t index) const
    {
        return const_cast<DisjointSpans*>(this)->find(index);
    }

    size_t size() const
    {
        size_t size = 0;
        for (auto& span : m_spans)
            size += span.size();
        return size;
    }

    bool is_empty() const
    {
        return all_of(m_spans, [](auto& span) { return span.is_empty(); });
    }

    DisjointSpans slice(size_t start, size_t length) const
    {
        DisjointSpans spans;
        for (auto& span : m_spans) {
            if (length == 0)
                break;
            if (start >= span.size()) {
                start -= span.size();
                continue;
            }

            auto sliced_length = min(length, span.size() - start);
            spans.m_spans.append(span.slice(start, sliced_length));
            start = 0;
            length -= sliced_length;
        }
        // Make sure that we weren't asked to make a slice larger than possible.
        VERIFY(length == 0);
        return spans;
    }
    DisjointSpans slice(size_t start) const { return slice(start, size() - start); }
    DisjointSpans slice_from_end(size_t length) const { return slice(size() - length, length); }

    DisjointIterator<Span<T>, false> begin() { return { m_spans }; }
    DisjointIterator<Span<T>, false> end() { return { m_spans, {} }; }
    DisjointIterator<Span<T>, true> begin() const { return { m_spans }; }
    DisjointIterator<Span<T>, true> end() const { return { m_spans, {} }; }

private:
    struct SpanAndOffset {
        Span<T>& span;
        size_t offset;
    };
    SpanAndOffset span_around(size_t index)
    {
        size_t offset = 0;
        for (auto& span : m_spans) {
            if (span.is_empty())
                continue;
            auto next_offset = span.size() + offset;
            if (next_offset <= index) {
                offset = next_offset;
                continue;
            }

            return { span, index - offset };
        }

        return { m_spans.last(), index - (offset - m_spans.last().size()) };
    }

    SpanContainer m_spans;
};

namespace Detail {

template<typename T, typename ChunkType>
ChunkType shatter_chunk(ChunkType& source_chunk, size_t start, size_t sliced_length)
{
    auto wanted_slice = source_chunk.span().slice(start, sliced_length);

    ChunkType new_chunk;
    if constexpr (IsTriviallyConstructible<T>) {
        new_chunk.resize(wanted_slice.size());

        TypedTransfer<T>::move(new_chunk.data(), wanted_slice.data(), wanted_slice.size());
    } else {
        new_chunk.ensure_capacity(wanted_slice.size());
        for (auto& entry : wanted_slice)
            new_chunk.unchecked_append(move(entry));
    }
    source_chunk.remove(start, sliced_length);
    return new_chunk;
}

template<typename T>
FixedArray<T> shatter_chunk(FixedArray<T>& source_chunk, size_t start, size_t sliced_length)
{
    auto wanted_slice = source_chunk.span().slice(start, sliced_length);

    FixedArray<T> new_chunk = FixedArray<T>::must_create_but_fixme_should_propagate_errors(wanted_slice.size());
    if constexpr (IsTriviallyConstructible<T>) {
        TypedTransfer<T>::move(new_chunk.data(), wanted_slice.data(), wanted_slice.size());
    } else {
        auto copied_chunk = FixedArray<T>::create(wanted_slice).release_value_but_fixme_should_propagate_errors();
        new_chunk.swap(copied_chunk);
    }
    auto rest_of_chunk = FixedArray<T>::create(source_chunk.span().slice(start)).release_value_but_fixme_should_propagate_errors();
    source_chunk.swap(rest_of_chunk);
    return new_chunk;
}

}

template<typename T, typename ChunkType = Vector<T>>
class DisjointChunks {
private:
    constexpr static auto InlineCapacity = IsCopyConstructible<ChunkType> ? 1 : 0;

public:
    DisjointChunks() = default;
    ~DisjointChunks() = default;
    DisjointChunks(DisjointChunks const&) = default;
    DisjointChunks(DisjointChunks&&) = default;

    DisjointChunks& operator=(DisjointChunks&&) = default;
    DisjointChunks& operator=(DisjointChunks const&) = default;

    void append(ChunkType&& chunk) { m_chunks.append(move(chunk)); }
    void extend(DisjointChunks&& chunks) { m_chunks.extend(move(chunks.m_chunks)); }
    void extend(DisjointChunks const& chunks) { m_chunks.extend(chunks.m_chunks); }

    ChunkType& first_chunk() { return m_chunks.first(); }
    ChunkType& last_chunk() { return m_chunks.last(); }
    ChunkType const& first_chunk() const { return m_chunks.first(); }
    ChunkType const& last_chunk() const { return m_chunks.last(); }

    void ensure_capacity(size_t needed_capacity)
    {
        m_chunks.ensure_capacity(needed_capacity);
    }

    void insert(size_t index, T value)
    {
        if (m_chunks.size() == 1)
            return m_chunks.first().insert(index, value);
        auto chunk_and_offset = chunk_around(index);
        if (!chunk_and_offset.chunk) {
            m_chunks.empend();
            chunk_and_offset.chunk = &m_chunks.last();
        }

        chunk_and_offset.chunk->insert(chunk_and_offset.offset, move(value));
    }

    void clear() { m_chunks.clear(); }

    T& operator[](size_t index) { return at(index); }
    T const& operator[](size_t index) const { return at(index); }
    T const& at(size_t index) const { return const_cast<DisjointChunks&>(*this).at(index); }
    T& at(size_t index)
    {
        auto value = find(index);
        VERIFY(value != nullptr);
        return *value;
    }

    T* find(size_t index)
    {
        if (m_chunks.size() == 1) {
            if (m_chunks.first().size() > index)
                return &m_chunks.first().at(index);
            return nullptr;
        }
        auto chunk_and_offset = chunk_around(index);
        if (!chunk_and_offset.chunk || chunk_and_offset.offset >= chunk_and_offset.chunk->size())
            return nullptr;
        return &chunk_and_offset.chunk->at(chunk_and_offset.offset);
    }

    T const* find(size_t index) const
    {
        return const_cast<DisjointChunks*>(this)->find(index);
    }

    size_t size() const
    {
        size_t sum = 0;
        for (auto& chunk : m_chunks)
            sum += chunk.size();
        return sum;
    }

    bool is_empty() const
    {
        return all_of(m_chunks, [](auto& chunk) { return chunk.is_empty(); });
    }

    template<size_t InlineSize = 0>
    DisjointSpans<T, Vector<Span<T>, InlineSize>> spans() const&
    {
        Vector<Span<T>, InlineSize> spans;
        spans.ensure_capacity(m_chunks.size());
        if (m_chunks.size() == 1) {
            spans.append(const_cast<ChunkType&>(m_chunks[0]).span());
            return DisjointSpans<T, Vector<Span<T>, InlineSize>> { move(spans) };
        }

        for (auto& chunk : m_chunks)
            spans.unchecked_append(const_cast<ChunkType&>(chunk).span());
        return DisjointSpans<T, Vector<Span<T>, InlineSize>> { move(spans) };
    }

    bool operator==(DisjointChunks const& other) const
    {
        if (other.size() != size())
            return false;

        auto it = begin();
        auto other_it = other.begin();
        for (; it != end(); ++it, ++other_it) {
            if (*it != *other_it)
                return false;
        }
        return true;
    }

    [[nodiscard]] ReadonlySpan<T> flat_data() const
    {
        VERIFY(m_chunks.size() <= 1);
        return m_chunks.is_empty() ? ReadonlySpan<T> {} : m_chunks.first().span();
    }

    DisjointChunks release_slice(size_t start, size_t length) & { return move(*this).slice(start, length); }
    DisjointChunks release_slice(size_t start) & { return move(*this).slice(start); }

    DisjointChunks slice(size_t start, size_t length) &&
    {
        DisjointChunks result;
        for (auto& chunk : m_chunks) {
            if (length == 0)
                break;
            if (start >= chunk.size()) {
                start -= chunk.size();
                continue;
            }

            auto sliced_length = min(length, chunk.size() - start);
            if (start == 0 && sliced_length == chunk.size()) {
                // Happy path! move the chunk itself.
                result.m_chunks.append(move(chunk));
            } else {
                // Shatter the chunk, we were asked for only a part of it :(
                auto new_chunk = Detail::shatter_chunk<T>(chunk, start, sliced_length);

                result.m_chunks.append(move(new_chunk));
            }
            start = 0;
            length -= sliced_length;
        }

        m_chunks.remove_all_matching([](auto& chunk) { return chunk.is_empty(); });

        // Make sure that we weren't asked to make a slice larger than possible.
        VERIFY(length == 0);
        return result;
    }
    DisjointChunks slice(size_t start) && { return move(*this).slice(start, size() - start); }
    DisjointChunks slice_from_end(size_t length) && { return move(*this).slice(size() - length, length); }

    void flatten()
    {
        if (m_chunks.is_empty())
            return;

        auto size = this->size();
        auto& first_chunk = m_chunks.first();
        first_chunk.ensure_capacity(size);
        bool first = true;
        for (auto& chunk : m_chunks) {
            if (first) {
                first = false;
                continue;
            }

            first_chunk.extend(move(chunk));
        }
        m_chunks.remove(1, m_chunks.size() - 1);
    }

    DisjointIterator<ChunkType, false, InlineCapacity> begin() { return { m_chunks }; }
    DisjointIterator<ChunkType, false, InlineCapacity> end() { return { m_chunks, {} }; }
    DisjointIterator<ChunkType, true, InlineCapacity> begin() const { return { m_chunks }; }
    DisjointIterator<ChunkType, true, InlineCapacity> end() const { return { m_chunks, {} }; }

private:
    struct ChunkAndOffset {
        ChunkType* chunk;
        size_t offset;
    };
    ChunkAndOffset chunk_around(size_t index)
    {
        if (m_chunks.is_empty())
            return { nullptr, index };

        size_t offset = 0;
        for (auto& chunk : m_chunks) {
            if (chunk.is_empty())
                continue;
            auto next_offset = chunk.size() + offset;
            if (next_offset <= index) {
                offset = next_offset;
                continue;
            }

            return { &chunk, index - offset };
        }

        return { &m_chunks.last(), index - (offset - m_chunks.last().size()) };
    }

    Vector<ChunkType, InlineCapacity> m_chunks;
};

template<typename T>
struct Traits<DisjointSpans<T>> : public DefaultTraits<DisjointSpans<T>> {
    static unsigned hash(DisjointSpans<T> const& span)
    {
        unsigned hash = 0;
        for (auto const& value : span) {
            auto value_hash = Traits<T>::hash(value);
            hash = pair_int_hash(hash, value_hash);
        }
        return hash;
    }

    constexpr static bool is_trivial() { return false; }
};

}
#if USING_AK_GLOBALLY
using AK::DisjointChunks;
using AK::DisjointSpans;
#endif
