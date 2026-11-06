/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Traits.h>
#include <AK/Utf16String.h>
#include <LibJS/ScriptCompilation.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// What a compiled script is stored under. The filename, base URL and muted errors of a script belong to one load of it
// and are not part of its compiled code.
struct CompiledScriptCacheKey {
    Utf16String partition;
    String url_without_fragment;
    JS::ProgramType program_type { JS::ProgramType::Script };
    size_t line_number_offset { 0 };

    bool operator==(CompiledScriptCacheKey const&) const = default;
};

// The compiled scripts of this process, which a script that loads again links into its own realm instead of being
// parsed and compiled again. Entries are found after a fetch completed, by the decoded source text the fetch came up
// with, so they answer nothing that the checks of a fetch decide, and can not stand in for different source text.
class WEB_API CompiledScriptCache {
public:
    static CompiledScriptCache& the();

    Optional<JS::CompiledProgram> find(CompiledScriptCacheKey const&, Utf16String const& source_text);

    // From here on, the program keeps the functions that get compiled for it. Replaces what the key held before.
    void store(CompiledScriptCacheKey, Utf16String source_text, JS::CompiledProgram const&);

    bool contains(CompiledScriptCacheKey const& key) const { return m_entries.contains(key); }

    void clear();

private:
    struct Entry {
        Utf16String source_text;
        JS::CompiledProgram program;
        u64 touch_serial { 0 };
    };

    static void release(Entry&);

    void evict_until_within_limit();

    HashMap<CompiledScriptCacheKey, Entry> m_entries;
    u64 m_next_touch_serial { 0 };
};

}

template<>
struct AK::Traits<Web::HTML::CompiledScriptCacheKey> : public AK::DefaultTraits<Web::HTML::CompiledScriptCacheKey> {
    static unsigned hash(Web::HTML::CompiledScriptCacheKey const& key)
    {
        auto hash = pair_int_hash(key.partition.hash(), key.url_without_fragment.hash());
        hash = pair_int_hash(hash, to_underlying(key.program_type));
        return pair_int_hash(hash, u64_hash(key.line_number_offset));
    }
};
