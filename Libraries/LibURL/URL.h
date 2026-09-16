/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/GenericShorthands.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibURL/BlobURLEntry.h>
#include <LibURL/Host.h>
#include <LibURL/Origin.h>
#include <LibURL/RustFFI.h>

// On Linux distros that use mlibc `basename` is defined as a macro that expands to `__mlibc_gnu_basename` or `__mlibc_gnu_basename_c`, so we undefine it.
#if defined(AK_OS_LINUX) && defined(basename)
#    undef basename
#endif

namespace URL {

enum class PercentEncodeSet {
    C0Control,
    Fragment,
    Query,
    SpecialQuery,
    Path,
    Userinfo,
    Component,
    ApplicationXWWWFormUrlencoded,
};

enum class ExcludeFragment {
    No,
    Yes
};

void append_percent_encoded_if_necessary(StringBuilder&, u32 code_point, PercentEncodeSet set = PercentEncodeSet::Userinfo);
void append_percent_encoded(StringBuilder&, u32 code_point);
bool code_point_is_in_percent_encode_set(u32 code_point, PercentEncodeSet);
Optional<u16> default_port_for_scheme(StringView);
ReadonlySpan<StringView> special_schemes();
bool is_special_scheme(StringView);

enum class SpaceAsPlus {
    No,
    Yes,
};
String percent_encode(StringView input, PercentEncodeSet set = PercentEncodeSet::Userinfo, SpaceAsPlus = SpaceAsPlus::No);
String percent_encode(Utf16View input, PercentEncodeSet set = PercentEncodeSet::Userinfo, SpaceAsPlus = SpaceAsPlus::No);
ByteString percent_decode(StringView input);

// https://url.spec.whatwg.org/#url-representation
// A URL is a struct that represents a universal identifier. To disambiguate from a valid URL string it can also be referred to as a URL record.
class URL {
public:
    class PathSegments {
    public:
        class Iterator {
        public:
            StringView operator*() const { return m_path.substring_view(m_start, segment_end() - m_start); }
            Iterator& operator++();
            bool operator==(Iterator const& other) const { return m_start == other.m_start; }

        private:
            friend class PathSegments;

            Iterator(StringView path, bool is_opaque, size_t start)
                : m_path(path)
                , m_is_opaque(is_opaque)
                , m_start(start)
            {
            }

            size_t segment_end() const;

            StringView m_path;
            bool m_is_opaque { false };
            size_t m_start { 0 };
        };

        Iterator begin() const { return { m_path, m_is_opaque, m_is_opaque ? 0uz : 1uz }; }
        Iterator end() const { return { m_path, m_is_opaque, m_path.length() + 1 }; }

        size_t size() const;
        bool is_empty() const { return size() == 0; }
        StringView first() const;
        StringView last() const;

    private:
        friend class URL;

        PathSegments(StringView path, bool is_opaque)
            : m_path(path)
            , m_is_opaque(is_opaque)
        {
        }

        StringView m_path;
        bool m_is_opaque { false };
    };

    // FIXME: We should get rid of the default constructor, all URLs should be constructed through the Parser.
    URL() = default;

    StringView scheme() const { return view(0, m_components.scheme_end); }
    StringView username() const;
    StringView password() const;
    Optional<Host> host() const;
    StringView serialized_host() const;
    ByteString basename() const;
    Optional<StringView> query() const;
    Optional<StringView> fragment() const;
    Optional<u16> port() const;
    ByteString path_segment_at_index(size_t index) const;
    size_t path_segment_count() const { return path_segments().size(); }
    PathSegments path_segments() const { return { serialize_path(), has_an_opaque_path() }; }

    u16 port_or_default() const { return port().value_or(default_port_for_scheme(scheme()).value_or(0)); }

    // https://url.spec.whatwg.org/#url-opaque-path
    // A URL has an opaque path if its path is a URL path segment.
    bool has_an_opaque_path() const { return m_components.has_opaque_path; }

    bool cannot_have_a_username_or_password_or_port() const;

    bool includes_credentials() const;
    bool is_special() const { return m_components.scheme_type != FFI::SchemeType::NotSpecial; }

    void set_scheme(StringView);
    void set_username(StringView);
    void set_username(Utf16View);
    void set_password(StringView);
    void set_password(Utf16View);
    void set_host(Host const&);
    void set_port(Optional<u16>);
    void set_path(ReadonlySpan<StringView> percent_encoded_segments);
    void set_opaque_path(StringView percent_encoded_path);
    void set_query(Optional<StringView>);
    void set_fragment(Optional<StringView>);

    StringView serialize_path() const;
    ByteString file_path() const;
    String const& serialize() const { return m_serialization; }
    String serialize(ExcludeFragment) const;
    ByteString serialize_for_display() const;
    ByteString to_byte_string() const { return m_serialization.to_byte_string(); }
    String const& to_string() const { return m_serialization; }

    Origin origin() const;

    bool equals(URL const& other, ExcludeFragment = ExcludeFragment::No) const;

    Optional<URL> complete_url(StringView) const;

    [[nodiscard]] bool operator==(URL const& other) const { return m_serialization == other.m_serialization; }

    Optional<BlobURLEntry const&> blob_url_entry() const;
    void set_blob_url_entry(Optional<BlobURLEntry>);

    static URL about(StringView path);

    FFI::RustUrl to_rust() const;
    void set_from_rust(FFI::RustUrl const&);
    static void set_from_rust_callback(void* url, FFI::RustUrl const*);

private:
    StringView view(u32 start, u32 end) const { return m_serialization.bytes_as_string_view().substring_view(start, end - start); }
    u32 path_end() const;

    struct BlobURLEntryStorage : public RefCounted<BlobURLEntryStorage> {
        explicit BlobURLEntryStorage(BlobURLEntry entry)
            : entry(move(entry))
        {
        }

        BlobURLEntry entry;
    };

    String m_serialization;
    FFI::UrlComponents m_components {
        .scheme_end = 0,
        .username_end = 0,
        .host_start = 0,
        .host_end = 0,
        .path_start = 0,
        .query_start = FFI::URL_OFFSET_NONE,
        .fragment_start = FFI::URL_OFFSET_NONE,
        .port = 0,
        .has_port = false,
        .host_kind = FFI::HostKind::Null,
        .scheme_type = FFI::SchemeType::NotSpecial,
        .has_opaque_path = false,
    };

    // https://url.spec.whatwg.org/#concept-url-blob-entry
    // A URL also has an associated blob URL entry that is either null or a blob URL entry. It is initially null.
    RefPtr<BlobURLEntryStorage const> m_blob_url_entry;
};

void set_file_scheme_urls_have_tuple_origins();
bool file_scheme_urls_have_tuple_origins();

Optional<URL> create_with_url_or_path(ByteString const&);
Optional<URL> create_with_file_scheme(ByteString const& path, ByteString const& fragment = {}, ByteString const& hostname = {});
URL create_with_data(StringView mime_type, StringView payload, bool is_base64 = false);

inline URL about_blank() { return URL::about("blank"sv); }
inline URL about_srcdoc() { return URL::about("srcdoc"sv); }
inline URL about_error() { return URL::about("error"sv); }

}

template<>
struct AK::Formatter<URL::URL> : AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, URL::URL const& value)
    {
        return Formatter<StringView>::format(builder, value.serialize());
    }
};

template<>
struct AK::Traits<URL::URL> : public AK::DefaultTraits<URL::URL> {
    static unsigned hash(URL::URL const& url) { return url.serialize().hash(); }
};
