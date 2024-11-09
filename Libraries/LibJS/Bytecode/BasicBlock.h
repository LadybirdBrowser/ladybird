/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/String.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/ScopedOperand.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Handle.h>

namespace JS::Bytecode {

struct UnwindInfo {
    JS::GCPtr<Executable const> executable;
    JS::GCPtr<Environment> lexical_environment;

    bool handler_called { false };
};

class BasicBlock {
    AK_MAKE_NONCOPYABLE(BasicBlock);

public:
    static NonnullOwnPtr<BasicBlock> create(u32 index, String name);
    ~BasicBlock();

    u32 index() const { return m_index; }

    ReadonlyBytes instruction_stream() const { return m_buffer.span(); }
    u8* data() { return m_buffer.data(); }
    u8 const* data() const { return m_buffer.data(); }
    size_t size() const { return m_buffer.size(); }

    void rewind()
    {
        m_buffer.resize_and_keep_capacity(m_last_instruction_start_offset);
        m_terminated = false;
    }

    void grow(size_t additional_size);

    void terminate(Badge<Generator>) { m_terminated = true; }
    bool is_terminated() const { return m_terminated; }

    String const& name() const { return m_name; }

    void set_handler(BasicBlock const& handler) { m_handler = &handler; }
    void set_finalizer(BasicBlock const& finalizer) { m_finalizer = &finalizer; }

    BasicBlock const* handler() const { return m_handler; }
    BasicBlock const* finalizer() const { return m_finalizer; }

    auto const& source_map() const { return m_source_map; }
    void add_source_map_entry(size_t bytecode_offset, SourceRecord const& source_record) { m_source_map.set(bytecode_offset, source_record); }

    [[nodiscard]] bool has_resolved_this() const { return m_has_resolved_this; }
    void set_has_resolved_this() { m_has_resolved_this = true; }

    [[nodiscard]] size_t last_instruction_start_offset() const { return m_last_instruction_start_offset; }
    void set_last_instruction_start_offset(size_t offset) { m_last_instruction_start_offset = offset; }

private:
    explicit BasicBlock(u32 index, String name);

    u32 m_index { 0 };
    Vector<u8> m_buffer;
    BasicBlock const* m_handler { nullptr };
    BasicBlock const* m_finalizer { nullptr };
    String m_name;
    bool m_terminated { false };
    bool m_has_resolved_this { false };

    HashMap<size_t, SourceRecord> m_source_map;

    size_t m_last_instruction_start_offset { 0 };
};

}
