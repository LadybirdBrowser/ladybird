/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Result.h>
#include <LibJS/Runtime/Object.h>
#include <LibRegex/Regex.h>

namespace JS {

ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_create(VM&, Value pattern, Value flags);
ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_alloc(VM&, FunctionObject& new_target);

Result<regex::RegexOptions<ECMAScriptFlags>, ByteString> regex_flags_from_string(StringView flags);
struct ParseRegexPatternError {
    ByteString error;
};
ErrorOr<ByteString, ParseRegexPatternError> parse_regex_pattern(StringView pattern, bool unicode, bool unicode_sets);
ThrowCompletionOr<ByteString> parse_regex_pattern(VM& vm, StringView pattern, bool unicode, bool unicode_sets);

class RegExpObject : public Object {
    JS_OBJECT(RegExpObject, Object);
    GC_DECLARE_ALLOCATOR(RegExpObject);

public:
    // JS regexps are all 'global' by default as per our definition, but the "global" flag enables "stateful".
    // FIXME: Enable 'BrowserExtended' only if in a browser context.
    static constexpr regex::RegexOptions<ECMAScriptFlags> default_flags {
        (regex::ECMAScriptFlags)regex::AllFlags::SingleMatch
        | (regex::ECMAScriptFlags)regex::AllFlags::Global
        | (regex::ECMAScriptFlags)regex::AllFlags::SkipTrimEmptyMatches
        | regex::ECMAScriptFlags::BrowserExtended
    };

    static GC::Ref<RegExpObject> create(Realm&);
    static GC::Ref<RegExpObject> create(Realm&, Regex<ECMA262> regex, ByteString pattern, ByteString flags);

    ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_initialize(VM&, Value pattern, Value flags);
    ByteString escape_regexp_pattern() const;

    virtual void initialize(Realm&) override;
    virtual ~RegExpObject() override = default;

    ByteString const& pattern() const { return m_pattern; }
    ByteString const& flags() const { return m_flags; }
    Regex<ECMA262> const& regex() { return *m_regex; }
    Regex<ECMA262> const& regex() const { return *m_regex; }
    Realm& realm() { return *m_realm; }
    Realm const& realm() const { return *m_realm; }
    bool legacy_features_enabled() const { return m_legacy_features_enabled; }
    void set_legacy_features_enabled(bool legacy_features_enabled) { m_legacy_features_enabled = legacy_features_enabled; }
    void set_realm(Realm& realm) { m_realm = &realm; }

private:
    RegExpObject(Object& prototype);
    RegExpObject(Regex<ECMA262> regex, ByteString pattern, ByteString flags, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    ByteString m_pattern;
    ByteString m_flags;
    bool m_legacy_features_enabled { false }; // [[LegacyFeaturesEnabled]]
    // Note: This is initialized in RegExpAlloc, but will be non-null afterwards
    GC::Ptr<Realm> m_realm; // [[Realm]]
    Optional<Regex<ECMA262>> m_regex;
};

}
