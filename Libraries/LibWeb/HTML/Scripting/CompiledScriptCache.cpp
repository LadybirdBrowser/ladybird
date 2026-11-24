/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibWeb/HTML/Scripting/CompiledScriptCache.h>

namespace Web::HTML {

static constexpr size_t compiled_script_cache_size_limit = 16 * MiB;

CompiledScriptFingerprint CompiledScriptFingerprint::of(Utf16String const& source_text)
{
    // NB: The storage width is part of what is hashed, so the same text in another width only misses the cache.
    auto view = source_text.utf16_view();
    u8 is_wide = view.has_latin1_storage() ? 0 : 1;
    Crypto::Hash::Hasher hasher { Crypto::Hash::HashKind::SHA256 };
    hasher.update(ReadonlyBytes { &is_wide, sizeof(is_wide) });
    if (is_wide) {
        auto units = view.utf16_span();
        hasher.update(ReadonlyBytes { reinterpret_cast<u8 const*>(units.data()), units.size() * sizeof(char16_t) });
    } else {
        hasher.update(view.latin1_span());
    }
    auto digest = hasher.finish();

    CompiledScriptFingerprint fingerprint;
    VERIFY(digest.size == fingerprint.sha256_digest.size());
    digest.bytes().copy_to(fingerprint.sha256_digest);
    fingerprint.length_in_code_units = source_text.length_in_code_units();
    return fingerprint;
}

CompiledScriptCache& CompiledScriptCache::the()
{
    static thread_local NeverDestroyed<CompiledScriptCache> cache;
    return *cache;
}

Optional<CompiledScriptCache::Hit> CompiledScriptCache::find(CompiledScriptCacheKey const& key, CompiledScriptFingerprint const& fingerprint)
{
    auto entry = m_entries.get(key);
    if (!entry.has_value() || entry->fingerprint != fingerprint)
        return {};

    entry->touch_serial = ++m_next_touch_serial;
    Hit hit { entry->program.clone(), nullptr };
    // NB: Loads of one script share one text, which LibJS parks for all of them at once.
    for (auto const& linked : entry->linked_source_codes) {
        if (auto source_code = linked.strong_ref()) {
            hit.source_code_to_share = move(source_code);
            break;
        }
    }

    // NB: Entries grow as their functions compile, which no store sees.
    evict_until_within_limit(key);
    return hit;
}

void CompiledScriptCache::store(CompiledScriptCacheKey key, CompiledScriptFingerprint const& fingerprint, JS::CompiledProgram const& program)
{
    if (auto entry = m_entries.take(key); entry.has_value())
        release(*entry);

    // NB: However large the program is, it costs nothing while a document runs it, which the one that stores it is about
    //     to, and goes once none does.
    program.set_retains_compiled_functions(true);
    m_entries.set(key, Entry { fingerprint, program.clone(), ++m_next_touch_serial, {} });
    evict_until_within_limit(key);
}

void CompiledScriptCache::note_linked(CompiledScriptCacheKey const& key, JS::SourceCode const& source_code)
{
    auto entry = m_entries.get(key);
    if (!entry.has_value())
        return;
    entry->linked_source_codes.remove_all_matching([](auto const& linked) { return !linked.is_alive(); });
    entry->linked_source_codes.append(JS::WeakSourceCode { source_code });
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

bool CompiledScriptCache::is_linked(Entry& entry)
{
    entry.linked_source_codes.remove_all_matching([](auto const& linked) { return !linked.is_alive(); });
    return !entry.linked_source_codes.is_empty();
}

void CompiledScriptCache::evict_until_within_limit(Optional<CompiledScriptCacheKey const&> about_to_be_linked)
{
    auto counts_against_limit = [&](auto& it) {
        if (about_to_be_linked.has_value() && it.key == *about_to_be_linked)
            return false;
        return !is_linked(it.value);
    };

    size_t size = 0;
    for (auto& it : m_entries) {
        if (counts_against_limit(it))
            size += it.value.program.memory_size();
    }

    while (size > compiled_script_cache_size_limit) {
        Optional<CompiledScriptCacheKey const&> least_recently_used_key;
        u64 least_recently_used_serial = NumericLimits<u64>::max();
        for (auto& it : m_entries) {
            if (it.value.touch_serial >= least_recently_used_serial || !counts_against_limit(it))
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
