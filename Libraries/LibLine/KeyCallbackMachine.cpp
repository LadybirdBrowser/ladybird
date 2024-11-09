/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibLine/Editor.h>

namespace Line {

void KeyCallbackMachine::register_key_input_callback(Vector<Key> keys, Function<bool(Editor&)> callback)
{
    m_key_callbacks.set(keys, make<KeyCallback>(move(callback)));
}

void KeyCallbackMachine::key_pressed(Editor& editor, Key key)
{
    dbgln_if(CALLBACK_MACHINE_DEBUG, "Key<{}, {}> pressed, seq_length={}, {} things in the matching vector", key.key, key.modifiers, m_sequence_length, m_current_matching_keys.size());
    if (m_sequence_length == 0) {
        VERIFY(m_current_matching_keys.is_empty());

        for (auto& it : m_key_callbacks) {
            if (it.key.first() == key)
                m_current_matching_keys.append(it.key);
        }

        if (m_current_matching_keys.is_empty()) {
            m_should_process_this_key = true;
            return;
        }
    }

    ++m_sequence_length;
    Vector<Vector<Key>> old_matching_keys;
    swap(m_current_matching_keys, old_matching_keys);

    for (auto& okey : old_matching_keys) {
        if (okey.size() < m_sequence_length)
            continue;

        if (okey[m_sequence_length - 1] == key)
            m_current_matching_keys.append(okey);
    }

    if (m_current_matching_keys.is_empty()) {
        // Insert any keys that were captured
        if (!old_matching_keys.is_empty()) {
            auto& keys = old_matching_keys.first();
            for (size_t i = 0; i < m_sequence_length - 1; ++i)
                editor.insert(keys[i].key);
        }
        m_sequence_length = 0;
        m_should_process_this_key = true;
        return;
    }

    if constexpr (CALLBACK_MACHINE_DEBUG) {
        dbgln("seq_length={}, matching vector:", m_sequence_length);
        for (auto& key : m_current_matching_keys) {
            for (auto& k : key)
                dbgln("    {}, {}", k.key, k.modifiers);
            dbgln("");
        }
    }

    m_should_process_this_key = false;
    for (auto& key : m_current_matching_keys) {
        if (key.size() == m_sequence_length) {
            m_should_process_this_key = m_key_callbacks.get(key).value()->callback(editor);
            m_sequence_length = 0;
            m_current_matching_keys.clear();
            return;
        }
    }
}

void KeyCallbackMachine::interrupted(Editor& editor)
{
    m_sequence_length = 0;
    m_current_matching_keys.clear();
    if (auto callback = m_key_callbacks.get({ ctrl('C') }); callback.has_value())
        m_should_process_this_key = callback.value()->callback(editor);
    else
        m_should_process_this_key = true;
}

}
