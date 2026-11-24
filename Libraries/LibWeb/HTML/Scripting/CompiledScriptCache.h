/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Traits.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/ScriptCompilation.h>
#include <LibJS/SourceCode.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// What tells the source text of an entry apart from other text, without keeping the text.
struct WEB_API CompiledScriptFingerprint {
    static CompiledScriptFingerprint of(Utf16String const& source_text);

    Array<u8, 32> sha256_digest {};
    size_t length_in_code_units { 0 };

    bool operator==(CompiledScriptFingerprint const&) const = default;
};

// What a compiled script is stored under. The filename, base URL and muted errors of a script belong to one load of it
// and are not part of its compiled code.
struct CompiledScriptCacheKey {
    Utf16String partition;
    String url_without_fragment;
    JS::ProgramType program_type { JS::ProgramType::Script };
    size_t line_number_offset { 0 };

    bool operator==(CompiledScriptCacheKey const&) const = default;
};

// The compiled scripts of this agent, which a script that loads again links into its own realm instead of being
// parsed and compiled again. Entries are found after a fetch completed, by the decoded source text the fetch came up
// with, so they answer nothing that the checks of a fetch decide, and can not stand in for different source text.
// Entries hold a SHA-256 digest of the text and not the text itself, so that the text goes once LibJS parks it.
//
// The size limit is for the programs that only the cache keeps alive. A program that a live document runs stays in
// memory with or without its entry, so it counts for nothing until no document runs it anymore, and an entry that the
// limit evicts is the one that no document runs and that was used the longest time ago.
class WEB_API CompiledScriptCache {
public:
    struct Hit {
        JS::CompiledProgram program;
        // A live source code that the program was linked into, whose code a new load shares, if there is one.
        RefPtr<JS::SourceCode const> source_code_to_share;
    };

    static CompiledScriptCache& the();

    Optional<Hit> find(CompiledScriptCacheKey const&, CompiledScriptFingerprint const&);

    // From here on, the program keeps the functions that get compiled for it. Replaces what the key held before.
    void store(CompiledScriptCacheKey, CompiledScriptFingerprint const&, JS::CompiledProgram const&);

    // The program of the key is linked into a record of the source code, which keeps the entry from counting against
    // the limit for as long as anything holds the source code.
    void note_linked(CompiledScriptCacheKey const&, JS::SourceCode const&);

    bool contains(CompiledScriptCacheKey const& key) const { return m_entries.contains(key); }

    void clear();

private:
    struct Entry {
        CompiledScriptFingerprint fingerprint;
        JS::CompiledProgram program;
        u64 touch_serial { 0 };
        // The source code of the records that the program was linked into.
        Vector<JS::WeakSourceCode> linked_source_codes;
    };

    static void release(Entry&);
    static bool is_linked(Entry&);

    // An entry that is about to be linked, which no record holds the source code of yet, counts as linked.
    void evict_until_within_limit(Optional<CompiledScriptCacheKey const&> about_to_be_linked = {});

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
