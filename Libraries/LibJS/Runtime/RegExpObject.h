/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Optional.h>
#include <AK/Result.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>
#include <LibRegex/ECMAScriptRegex.h>

namespace JS {

JS_API ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_create(VM&, Value pattern, Value flags);
ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_alloc(VM&, FunctionObject& new_target);

struct ParseRegexPatternError {
    String error;
};
ErrorOr<String, ParseRegexPatternError> parse_regex_pattern(Utf16View const& pattern, bool unicode, bool unicode_sets);
ThrowCompletionOr<String> parse_regex_pattern(VM& vm, Utf16View const& pattern, bool unicode, bool unicode_sets);

class JS_API RegExpObject : public Object {
    JS_OBJECT(RegExpObject, Object);
    GC_DECLARE_ALLOCATOR(RegExpObject);

public:
    enum class Flags {
        HasIndices = 1 << 0,
        Global = 1 << 1,
        IgnoreCase = 1 << 2,
        Multiline = 1 << 3,
        DotAll = 1 << 4,
        UnicodeSets = 1 << 5,
        Unicode = 1 << 6,
        Sticky = 1 << 7,
    };

    static GC::Ref<RegExpObject> create(Realm&);
    static GC::Ref<RegExpObject> create(Realm&, Utf16String pattern, Utf16String flags);

    ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_initialize(VM&, Value pattern, Value flags);
    String escape_regexp_pattern() const;

    virtual void initialize(Realm&) override;
    virtual ~RegExpObject() override = default;

    Utf16String const& pattern() const { return m_pattern; }
    Utf16String const& flags() const { return m_flags; }
    Flags flag_bits() const { return m_flag_bits; }
    Realm& realm() { return *m_realm; }
    Realm const& realm() const { return *m_realm; }
    bool legacy_features_enabled() const { return m_legacy_features_enabled; }
    void set_legacy_features_enabled(bool legacy_features_enabled) { m_legacy_features_enabled = legacy_features_enabled; }
    void set_realm(Realm& realm) { m_realm = &realm; }

    regex::ECMAScriptRegex const* cached_regex() const { return m_cached_regex; }
    void set_cached_regex(regex::ECMAScriptRegex const* ptr) const { m_cached_regex = ptr; }

private:
    RegExpObject(Object& prototype);
    RegExpObject(Utf16String pattern, Utf16String flags, Object& prototype);

    virtual bool is_regexp_object() const final { return true; }
    virtual void visit_edges(Visitor&) override;

    Utf16String m_pattern;
    Utf16String m_flags;
    Flags m_flag_bits { 0 };
    bool m_legacy_features_enabled { false }; // [[LegacyFeaturesEnabled]]
    mutable regex::ECMAScriptRegex const* m_cached_regex { nullptr };
    // Note: This is initialized in RegExpAlloc, but will be non-null afterwards
    GC::Ptr<Realm> m_realm; // [[Realm]]
};

template<>
inline bool Object::fast_is<RegExpObject>() const { return is_regexp_object(); }

AK_ENUM_BITWISE_OPERATORS(RegExpObject::Flags);

}
