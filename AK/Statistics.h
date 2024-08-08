/*
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Math.h>
#include <AK/QuickSelect.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>

namespace AK {

static constexpr int ODD_NAIVE_MEDIAN_CUTOFF = 200;
static constexpr int EVEN_NAIVE_MEDIAN_CUTOFF = 350;

template<Arithmetic T = float, typename ContainerType = Vector<T>>
class Statistics {
public:
    Statistics() = default;
    ~Statistics() = default;

    explicit Statistics(ContainerType&& existing_container)
        : m_values(forward<ContainerType>(existing_container))
    {
        T c = 0.0; // correction term
        for (auto const& value : m_values) {
            T const y = value - c;
            T const t = m_sum + y;
            c = (t - m_sum) - y;
            m_sum = t;
        }
    }

    void add(T const& value)
    {
        // FIXME: Check for an overflow
        m_sum += value;
        m_values.append(value);
    }

    T const sum() const { return m_sum; }

    // FIXME: Unclear Wording, average can mean a lot of different things
    // Median, Arithmetic Mean (which this is), Geometric Mean, Harmonic Mean etc
    float average() const
    {
        // Let's assume the average of an empty dataset is 0
        if (size() == 0)
            return 0;

        // TODO: sum might overflow so maybe do multiple partial sums and intermediate divisions here
        return (float)sum() / size();
    }

    T const min() const
    {
        // Lets Rather fail than read over the end of a collection
        VERIFY(size() != 0);

        T minimum = m_values[0];
        for (T number : values()) {
            if (number < minimum) {
                minimum = number;
            }
        }
        return minimum;
    }

    T const max() const
    {
        // Lets Rather fail than read over the end of a collection
        VERIFY(size() != 0);

        T maximum = m_values[0];
        for (T number : values()) {
            if (number > maximum) {
                maximum = number;
            }
        }
        return maximum;
    }

    T const median()
    {
        // Let's assume the Median of an empty dataset is 0
        if (size() == 0)
            return 0;

        // If the number of values is even, the median is the arithmetic mean of the two middle values
        if (size() <= EVEN_NAIVE_MEDIAN_CUTOFF && size() % 2 == 0) {
            quick_sort(m_values);
            return (m_values.at(size() / 2) + m_values.at(size() / 2 - 1)) / 2;
        } else if (size() <= ODD_NAIVE_MEDIAN_CUTOFF && size() % 2 == 1) {
            quick_sort(m_values);
            return m_values.at(m_values.size() / 2);
        } else if (size() % 2 == 0) {
            auto index = size() / 2;
            auto median1 = m_values.at(AK::quickselect_inplace(m_values, index));
            auto median2 = m_values.at(AK::quickselect_inplace(m_values, index - 1));
            return (median1 + median2) / 2;
        }
        return m_values.at(AK::quickselect_inplace(m_values, size() / 2));
    }

    float standard_deviation() const { return sqrt(variance()); }
    float variance() const
    {
        float summation = 0;
        float avg = average();
        for (T number : values()) {
            float difference = (float)number - avg;
            summation += (difference * difference);
        }
        summation = summation / size();
        return summation;
    }

    ContainerType const& values() const { return m_values; }
    size_t size() const { return m_values.size(); }

private:
    ContainerType m_values;
    T m_sum {};
};

}

#if USING_AK_GLOBALLY
using AK::Statistics;
#endif
