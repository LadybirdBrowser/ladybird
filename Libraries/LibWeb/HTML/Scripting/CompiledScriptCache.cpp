/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibWeb/HTML/Scripting/CompiledScriptCache.h>

namespace Web::HTML {

static constexpr size_t compiled_script_cache_size_limit = 16 * MiB;

CompiledScriptCache& CompiledScriptCache::the()
{
    static thread_local NeverDestroyed<CompiledScriptCache> cache;
    return *cache;
}

Optional<CompiledScriptCache::Hit> CompiledScriptCache::find(CompiledScriptCacheKey const& key, Utf16String const& source_text)
{
    auto entry = m_entries.get(key);
    if (!entry.has_value() || entry->source_text != source_text)
        return {};

    entry->touch_serial = ++m_next_touch_serial;
    Hit hit { entry->program.clone(), entry->source_text };

    // NB: Entries grow as their functions compile, which no store sees.
    evict_until_within_limit();
    return hit;
}

void CompiledScriptCache::store(CompiledScriptCacheKey key, Utf16String source_text, JS::CompiledProgram const& program)
{
    if (auto entry = m_entries.take(key); entry.has_value())
        release(*entry);

    if (program.memory_size() <= compiled_script_cache_size_limit) {
        program.set_retains_compiled_functions(true);
        m_entries.set(move(key), Entry { move(source_text), program.clone(), ++m_next_touch_serial });
    }
    evict_until_within_limit();
}

void CompiledScriptCache::clear()
{
    for (auto& it : m_entries)
        release(it.value);
    m_entries.clear();
}

void CompiledScriptCache::release(Entry& entry)
{
    // NB: Realms that still run the program have no use for keeping the functions they compile from here on.
    entry.program.set_retains_compiled_functions(false);
}

void CompiledScriptCache::evict_until_within_limit()
{
    size_t size = 0;
    for (auto const& it : m_entries)
        size += it.value.program.memory_size();

    while (size > compiled_script_cache_size_limit) {
        Optional<CompiledScriptCacheKey const&> least_recently_used_key;
        u64 least_recently_used_serial = NumericLimits<u64>::max();
        for (auto const& it : m_entries) {
            if (it.value.touch_serial >= least_recently_used_serial)
                continue;
            least_recently_used_key = it.key;
            least_recently_used_serial = it.value.touch_serial;
        }
        VERIFY(least_recently_used_key.has_value());

        auto entry = m_entries.take(*least_recently_used_key);
        size -= entry->program.memory_size();
        release(*entry);
    }
}

}
