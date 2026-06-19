/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WEB_API ScriptRegistry {
public:
    struct Content {
        String content_type;
        String text;
    };

    struct Identifier {
        UniqueNodeID document_id;
        u64 script_id { 0 };

        bool operator==(Identifier const&) const = default;
    };

    struct Description {
        Identifier id;
        Optional<URL::URL> url;
        String display_url;
        String introduction_type;
        String content_type;
        bool is_inline_source { false };
        u32 source_start_line { 1 };
        u32 source_start_column { 0 };
        size_t source_length { 0 };
    };

    struct JavaScriptSource {
        NonnullRefPtr<JS::SourceCode const> source_code;
    };

    using ContentHandle = Variant<JavaScriptSource>;

    enum class IsInlineSource : u8 {
        No,
        Yes,
    };

    struct Script {
        Description description;
        ContentHandle content;
    };

    Script const& register_javascript_source(NonnullRefPtr<JS::SourceCode const>, ByteString const& filename, String introduction_type, IsInlineSource, size_t source_line_number, size_t source_length);

    OrderedHashMap<u64, Script> const& scripts() const { return m_scripts; }
    Optional<Content> script_content(u64 script_id, String const& document_source) const;

private:
    OrderedHashMap<u64, Script> m_scripts;
    u64 m_next_script_id { 1 };
};

}

template<>
struct AK::Traits<Web::HTML::ScriptRegistry::Identifier> : public AK::DefaultTraits<Web::HTML::ScriptRegistry::Identifier> {
    static bool equals(Web::HTML::ScriptRegistry::Identifier const& lhs, Web::HTML::ScriptRegistry::Identifier const& rhs)
    {
        return lhs == rhs;
    }

    static unsigned hash(Web::HTML::ScriptRegistry::Identifier const& identifier)
    {
        return pair_int_hash(identifier.document_id.value(), identifier.script_id);
    }
};

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ScriptRegistry::Identifier const&);

template<>
WEB_API ErrorOr<Web::HTML::ScriptRegistry::Identifier> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ScriptRegistry::Description const&);

template<>
WEB_API ErrorOr<Web::HTML::ScriptRegistry::Description> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ScriptRegistry::Content const&);

template<>
WEB_API ErrorOr<Web::HTML::ScriptRegistry::Content> decode(Decoder&);

}
