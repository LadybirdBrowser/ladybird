/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>

namespace Line {

class Editor;

struct Key {
    enum Modifier : int {
        None = 0,
        Alt = 1,
    };

    int modifiers { None };
    unsigned key { 0 };

    Key(unsigned c)
        : modifiers(None)
        , key(c) {};

    Key(unsigned c, int modifiers)
        : modifiers(modifiers)
        , key(c)
    {
    }

    bool operator==(Key const& other) const
    {
        return other.key == key && other.modifiers == modifiers;
    }
};

struct KeyCallback {
    KeyCallback(Function<bool(Editor&)> cb)
        : callback(move(cb))
    {
    }
    Function<bool(Editor&)> callback;
};

class KeyCallbackMachine {
public:
    void register_key_input_callback(Vector<Key>, Function<bool(Editor&)> callback);
    void key_pressed(Editor&, Key);
    void interrupted(Editor&);
    bool should_process_last_pressed_key() const { return m_should_process_this_key; }

private:
    HashMap<Vector<Key>, NonnullOwnPtr<KeyCallback>> m_key_callbacks;
    Vector<Vector<Key>> m_current_matching_keys;
    size_t m_sequence_length { 0 };
    bool m_should_process_this_key { true };
};

}

namespace AK {

template<>
struct Traits<Line::Key> : public DefaultTraits<Line::Key> {
    static constexpr bool is_trivial() { return true; }
    static unsigned hash(Line::Key k) { return pair_int_hash(k.key, k.modifiers); }
};

template<>
struct Traits<Vector<Line::Key>> : public DefaultTraits<Vector<Line::Key>> {
    static constexpr bool is_trivial() { return false; }
    static unsigned hash(Vector<Line::Key> const& ks)
    {
        unsigned h = 0;
        for (auto& k : ks)
            h ^= Traits<Line::Key>::hash(k);
        return h;
    }
};

}
