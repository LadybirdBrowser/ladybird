/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibWebView/Export.h>

namespace WebView {

struct ProfileSelection {
    Optional<ByteString> name {};
    Optional<ByteString> path {};
    bool temporary { false };
};

struct ProfileRoots {
    ByteString config;
    ByteString data;
    ByteString cache;
    ByteString runtime;
    ByteString temporary;
};

struct ProfilePaths {
    ByteString config;
    ByteString data;
    ByteString cache;
    ByteString runtime;
    ByteString identity;
};

class WEBVIEW_API Profile {
    AK_MAKE_NONCOPYABLE(Profile);

public:
    Profile(Profile&&);
    Profile& operator=(Profile&&);
    ~Profile();

    static ErrorOr<Profile> create(ProfileSelection const&, ProfileRoots const&);
    static ErrorOr<Profile> create_legacy(ProfileRoots const&);
    static ByteString routing_identifier(StringView canonical_profile_root);
    static bool is_valid_name(StringView);

    ProfilePaths const& paths() const { return m_paths; }
    StringView identifier() const { return m_identifier; }
    bool is_temporary() const { return m_temporary_root.has_value(); }

private:
    Profile(ByteString identifier, ProfilePaths, Optional<ByteString> temporary_root = {});
    void remove_temporary_root();

    ByteString m_identifier;
    ProfilePaths m_paths;
    Optional<ByteString> m_temporary_root;
};

}
