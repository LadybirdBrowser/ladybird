/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Gfx {

template<typename T>
class DisjointRectSet {
public:
    DisjointRectSet(DisjointRectSet const&) = delete;
    DisjointRectSet& operator=(DisjointRectSet const&) = delete;

    DisjointRectSet() = default;
    ~DisjointRectSet() = default;

    DisjointRectSet(Rect<T> const& rect)
    {
        m_rects.append(rect);
    }

    DisjointRectSet(DisjointRectSet&&) = default;
    DisjointRectSet& operator=(DisjointRectSet&&) = default;

    DisjointRectSet clone() const
    {
        DisjointRectSet rects;
        rects.m_rects = m_rects;
        return rects;
    }

    void move_by(T dx, T dy)
    {
        for (auto& r : m_rects)
            r.translate_by(dx, dy);
    }
    void move_by(Point<T> const& delta)
    {
        move_by(delta.x(), delta.y());
    }

    void add(Rect<T> const& rect)
    {
        if (add_no_shatter(rect) && m_rects.size() > 1)
            shatter();
    }

    template<typename Container>
    void add_many(Container const& rects)
    {
        bool added = false;
        for (auto const& rect : rects) {
            if (add_no_shatter(rect))
                added = true;
        }
        if (added && m_rects.size() > 1)
            shatter();
    }

    void add(DisjointRectSet const& rect_set)
    {
        if (this == &rect_set)
            return;
        if (m_rects.is_empty()) {
            m_rects = rect_set.m_rects;
        } else {
            add_many(rect_set.rects());
        }
    }

    DisjointRectSet shatter(Rect<T> const& hammer) const
    {
        if (hammer.is_empty())
            return clone();

        DisjointRectSet shards;
        for (auto& rect : m_rects) {
            for (auto& shard : rect.shatter(hammer))
                shards.add_no_shatter(shard);
        }
        // Since there should be no overlaps, we don't need to call shatter()
        return shards;
    }
    DisjointRectSet shatter(DisjointRectSet const& hammer) const
    {
        if (this == &hammer)
            return {};
        if (hammer.is_empty() || !intersects(hammer))
            return clone();

        // TODO: This could use some optimization
        DisjointRectSet shards = shatter(hammer.m_rects[0]);
        auto rects_count = hammer.m_rects.size();
        for (size_t i = 1; i < rects_count && !shards.is_empty(); i++) {
            if (hammer.m_rects[i].intersects(shards.m_rects)) {
                auto shattered = shards.shatter(hammer.m_rects[i]);
                shards = move(shattered);
            }
        }
        // Since there should be no overlaps, we don't need to call shatter()
        return shards;
    }

    bool contains(Rect<T> const& rect) const
    {
        if (is_empty() || rect.is_empty())
            return false;

        // TODO: This could use some optimization
        DisjointRectSet remainder(rect);
        for (auto& r : m_rects) {
            auto shards = remainder.shatter(r);
            if (shards.is_empty())
                return true;
            remainder = move(shards);
        }
        return false;
    }

    bool intersects(Rect<T> const& rect) const
    {
        for (auto& r : m_rects) {
            if (r.intersects(rect))
                return true;
        }
        return false;
    }
    bool intersects(DisjointRectSet const& rects) const
    {
        if (this == &rects)
            return true;

        for (auto& r : m_rects) {
            for (auto& r2 : rects.m_rects) {
                if (r.intersects(r2))
                    return true;
            }
        }
        return false;
    }

    DisjointRectSet intersected(Rect<T> const& rect) const
    {
        DisjointRectSet intersected_rects;
        intersected_rects.m_rects.ensure_capacity(m_rects.capacity());
        for (auto& r : m_rects) {
            auto intersected_rect = r.intersected(rect);
            if (!intersected_rect.is_empty())
                intersected_rects.m_rects.append(intersected_rect);
        }
        // Since there should be no overlaps, we don't need to call shatter()
        return intersected_rects;
    }
    DisjointRectSet intersected(DisjointRectSet const& rects) const
    {
        if (&rects == this)
            return clone();
        if (is_empty() || rects.is_empty())
            return {};

        DisjointRectSet intersected_rects;
        intersected_rects.m_rects.ensure_capacity(m_rects.capacity());
        for (auto& r : m_rects) {
            for (auto& r2 : rects.m_rects) {
                auto intersected_rect = r.intersected(r2);
                if (!intersected_rect.is_empty())
                    intersected_rects.m_rects.append(intersected_rect);
            }
        }
        // Since there should be no overlaps, we don't need to call shatter()
        return intersected_rects;
    }

    template<typename Function>
    IterationDecision for_each_intersected(Rect<T> const& rect, Function f) const
    {
        if (is_empty() || rect.is_empty())
            return IterationDecision::Continue;
        for (auto& r : m_rects) {
            auto intersected_rect = r.intersected(rect);
            if (intersected_rect.is_empty())
                continue;
            IterationDecision decision = f(intersected_rect);
            if (decision != IterationDecision::Continue)
                return decision;
        }
        return IterationDecision::Continue;
    }

    template<typename Function>
    IterationDecision for_each_intersected(DisjointRectSet const& rects, Function f) const
    {
        if (is_empty() || rects.is_empty())
            return IterationDecision::Continue;
        if (this == &rects) {
            for (auto& r : m_rects) {
                IterationDecision decision = f(r);
                if (decision != IterationDecision::Continue)
                    return decision;
            }
        } else {
            for (auto& r : m_rects) {
                for (auto& r2 : rects.m_rects) {
                    auto intersected_rect = r.intersected(r2);
                    if (intersected_rect.is_empty())
                        continue;
                    IterationDecision decision = f(intersected_rect);
                    if (decision != IterationDecision::Continue)
                        return decision;
                }
            }
        }
        return IterationDecision::Continue;
    }

    bool is_empty() const { return m_rects.is_empty(); }
    size_t size() const { return m_rects.size(); }

    void clear() { m_rects.clear(); }
    void clear_with_capacity() { m_rects.clear_with_capacity(); }
    Vector<Rect<T>, 32> const& rects() const { return m_rects; }
    Vector<Rect<T>, 32> take_rects() { return move(m_rects); }

    void translate_by(T dx, T dy)
    {
        for (auto& rect : m_rects)
            rect.translate_by(dx, dy);
    }
    void translate_by(Point<T> const& delta)
    {
        for (auto& rect : m_rects)
            rect.translate_by(delta);
    }

private:
    bool add_no_shatter(Rect<T> const& new_rect)
    {
        if (new_rect.is_empty())
            return false;
        for (auto& rect : m_rects) {
            if (rect.contains(new_rect))
                return false;
        }

        m_rects.append(new_rect);
        return true;
    }

    void shatter()
    {
        Vector<Rect<T>, 32> output;
        output.ensure_capacity(m_rects.size());
        bool pass_had_intersections = false;
        do {
            pass_had_intersections = false;
            output.clear_with_capacity();
            for (size_t i = 0; i < m_rects.size(); ++i) {
                auto& r1 = m_rects[i];
                for (size_t j = 0; j < m_rects.size(); ++j) {
                    if (i == j)
                        continue;
                    auto& r2 = m_rects[j];
                    if (!r1.intersects(r2))
                        continue;
                    pass_had_intersections = true;
                    auto pieces = r1.shatter(r2);
                    for (auto& piece : pieces)
                        output.append(piece);
                    m_rects.remove(i);
                    for (; i < m_rects.size(); ++i)
                        output.append(m_rects[i]);
                    goto next_pass;
                }
                output.append(r1);
            }
        next_pass:
            swap(output, m_rects);
        } while (pass_had_intersections);
    }

    Vector<Rect<T>, 32> m_rects;
};

}
