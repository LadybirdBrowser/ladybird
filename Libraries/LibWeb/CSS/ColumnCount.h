/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::CSS {

class ColumnCount {
public:
    enum class Type {
        Auto,
        Integer
    };

    static ColumnCount make_auto()
    {
        return ColumnCount();
    }

    static ColumnCount make_integer(int value)
    {
        return ColumnCount(value);
    }

    bool is_auto() const { return m_type == Type::Auto; }
    int value() const { return *m_value; }

private:
    ColumnCount(int value)
        : m_type(Type::Integer)
        , m_value(value)
    {
    }
    ColumnCount() {};

    Type m_type { Type::Auto };
    Optional<int> m_value;
};

}
