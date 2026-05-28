/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/NonnullRefPtr.h>
#include <LibJS/DecodedBytecodeCache.h>

namespace JS {

class Script;
class SourceTextModule;

// Tracks how a Script or SourceTextModule executable is backed. The owner keeps
// the concrete executable and shared-function-data storage; this object only
// owns backing state and transition rules.
class ExecutableBacking {
private:
    enum class State {
        Source,
        HeapBytecode,
        GeneratingFreshCacheFromSource,
        GeneratingFreshCacheFromHeapBytecode,
        MappedBytecodeCache,
    };

    explicit ExecutableBacking(State state)
        : m_state(state)
    {
    }

    static ExecutableBacking source()
    {
        return ExecutableBacking(State::Source);
    }

    static ExecutableBacking heap_bytecode()
    {
        return ExecutableBacking(State::HeapBytecode);
    }

    static ExecutableBacking mapped_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache> bytecode_cache)
    {
        auto backing = ExecutableBacking(State::MappedBytecodeCache);
        backing.m_bytecode_cache = move(bytecode_cache);
        return backing;
    }

public:
    [[nodiscard]] bool is_source() const
    {
        return m_state == State::Source
            || m_state == State::GeneratingFreshCacheFromSource;
    }

    [[nodiscard]] bool is_heap_bytecode() const
    {
        return m_state == State::HeapBytecode
            || m_state == State::GeneratingFreshCacheFromHeapBytecode;
    }

    [[nodiscard]] bool is_mapped_bytecode_cache() const { return m_state == State::MappedBytecodeCache; }

private:
    friend class Script;
    friend class SourceTextModule;

    [[nodiscard]] bool can_generate_bytecode_cache() const
    {
        return m_state == State::Source
            || m_state == State::HeapBytecode;
    }

    [[nodiscard]] bool can_install_generated_bytecode_cache() const { return is_generating_bytecode_cache(); }

    [[nodiscard]] bool requires_non_bytecode_cache_compile_inputs_to_be_cleared() const
    {
        return is_mapped_bytecode_cache();
    }

    [[nodiscard]] bool is_generating_bytecode_cache() const
    {
        return m_state == State::GeneratingFreshCacheFromSource
            || m_state == State::GeneratingFreshCacheFromHeapBytecode;
    }

    void begin_bytecode_cache_generation()
    {
        switch (m_state) {
        case State::Source:
            m_state = State::GeneratingFreshCacheFromSource;
            return;
        case State::HeapBytecode:
            m_state = State::GeneratingFreshCacheFromHeapBytecode;
            return;
        case State::GeneratingFreshCacheFromSource:
        case State::GeneratingFreshCacheFromHeapBytecode:
        case State::MappedBytecodeCache:
            VERIFY_NOT_REACHED();
        }
        VERIFY_NOT_REACHED();
    }

    void finish_bytecode_cache_generation_without_install()
    {
        switch (m_state) {
        case State::GeneratingFreshCacheFromSource:
            m_state = State::Source;
            return;
        case State::GeneratingFreshCacheFromHeapBytecode:
            m_state = State::HeapBytecode;
            return;
        case State::Source:
        case State::HeapBytecode:
        case State::MappedBytecodeCache:
            VERIFY_NOT_REACHED();
        }
        VERIFY_NOT_REACHED();
    }

    void finish_bytecode_cache_install(NonnullRefPtr<RustIntegration::DecodedBytecodeCache> bytecode_cache)
    {
        VERIFY(!is_mapped_bytecode_cache());
        m_state = State::MappedBytecodeCache;
        m_bytecode_cache = move(bytecode_cache);
    }

    State m_state { State::Source };
    RefPtr<RustIntegration::DecodedBytecodeCache> m_bytecode_cache;
};

}
