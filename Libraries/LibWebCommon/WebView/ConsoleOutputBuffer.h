/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DoublyLinkedList.h>
#include <AK/Function.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>

namespace WebView {

// The latest messages that a document's console output, which DevTools gets once it asks: Only the latest
// MAX_MESSAGES are kept, and older ones are dropped until the rest come to no more than about MAX_SIZE bytes.
class WEBCOMMON_API ConsoleOutputBuffer {
public:
    static constexpr size_t MAX_MESSAGES = 1000;
    static constexpr size_t MAX_SIZE = 10 * MiB;

    void append(ConsoleOutput);
    void for_each(Function<void(ConsoleOutput const&)> const&) const;

    size_t message_count() const { return m_message_count; }
    size_t size() const { return m_size; }

private:
    struct Entry {
        ConsoleOutput output;
        size_t size { 0 };
    };

    DoublyLinkedList<Entry> m_entries;
    size_t m_message_count { 0 };
    size_t m_size { 0 };
};

}
