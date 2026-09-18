/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <RequestServer/Sandbox.h>
#include <curl/curl.h>
#include <dlfcn.h>
#include <openssl/x509.h>
#include <string.h>

namespace RequestServer {

// Landlock rules bind to inodes. A rule on /etc/ssl therefore covers the files that live there, but not the
// targets of the symlinks distributions such as Fedora (/etc/pki) and Arch (/etc/ca-certificates) place in
// it. Opening the bundle path itself follows the link, so a rule on the exact file libcurl and OpenSSL open
// grants the real bundle wherever it lives, and nothing next to it.
static ErrorOr<void> add_certificate_store_paths(Vector<Sandbox::LandlockPath>& paths)
{
    if (auto const* version_info = curl_version_info(CURLVERSION_NOW)) {
        if (version_info->cainfo)
            TRY(Sandbox::add_landlock_path_if_exists(paths, ByteString { version_info->cainfo }, Sandbox::LandlockPath::Access::ReadOnly));
        if (version_info->capath)
            TRY(Sandbox::add_landlock_path_if_exists(paths, ByteString { version_info->capath }, Sandbox::LandlockPath::Access::ReadOnly));
    }

    // libcurl is built with CURL_CA_FALLBACK, so OpenSSL's default store is consulted when no bundle is set.
    TRY(Sandbox::add_landlock_path_if_exists(paths, ByteString { X509_get_default_cert_file() }, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, ByteString { X509_get_default_cert_dir() }, Sandbox::LandlockPath::Access::ReadOnly));
    for (auto variable : { X509_get_default_cert_file_env(), X509_get_default_cert_dir_env() }) {
        if (auto value = Core::Environment::get(StringView { variable, strlen(variable) }); value.has_value() && !value->is_empty())
            TRY(Sandbox::add_landlock_path_if_exists(paths, *value, Sandbox::LandlockPath::Access::ReadOnly));
    }

    return {};
}

static Vector<ByteString> nss_host_modules_from_nsswitch(StringView nsswitch_contents)
{
    Vector<ByteString> modules;
    for (auto line : nsswitch_contents.lines()) {
        auto trimmed = line.trim_whitespace();
        if (!trimmed.starts_with("hosts:"sv))
            continue;
        for (auto token : trimmed.substring_view("hosts:"sv.length()).split_view_if(is_ascii_space)) {
            // Action specifiers such as [NOTFOUND=return] and trailing comments are not modules.
            if (token.starts_with('['))
                continue;
            if (token.starts_with('#'))
                break;
            modules.append(token);
        }
        break;
    }
    return modules;
}

// glibc dlopens the NSS modules named on the hosts line of /etc/nsswitch.conf the first time getaddrinfo()
// runs, and LibDNS still resolves through getaddrinfo() unless a DNS server is configured. Load them now so
// the resolver keeps working once the sandbox denies the library directories and executable file mappings.
static void preload_nss_host_modules()
{
    auto file = Core::File::open("/etc/nsswitch.conf"sv, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return;

    for (auto const& module : nss_host_modules_from_nsswitch(contents.value())) {
        auto library_name = ByteString::formatted("libnss_{}.so.2", module);
        (void)dlopen(library_name.characters(), RTLD_LAZY | RTLD_GLOBAL | RTLD_NODELETE);
    }
}

ErrorOr<void> apply_sandbox(StringView, Vector<ByteString> const& certificates, StringView cache_path)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    preload_nss_host_modules();

    Vector<Sandbox::LandlockPath> paths;
    TRY(Core::Directory::create(cache_path, Core::Directory::CreateDirectories::Yes));

    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/ssl"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(add_certificate_store_paths(paths));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/host.conf"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/hosts"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/nsswitch.conf"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/resolv.conf"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/run/systemd/resolve"sv, Sandbox::LandlockPath::Access::ReadOnly));
    // nss-resolve checks this sysctl to decide whether to ask for AAAA records.
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/sys/net/ipv6/conf/all/disable_ipv6"sv, Sandbox::LandlockPath::Access::ReadOnly));

    for (auto const& certificate : certificates) {
        auto certificate_path = LexicalPath::dirname(certificate);
        if (certificate_path.is_empty())
            certificate_path = ".";

        TRY(Sandbox::add_landlock_path_if_exists(paths, certificate_path, Sandbox::LandlockPath::Access::ReadOnly));
    }

    if (g_resource_substitution_map) {
        TRY(g_resource_substitution_map->for_each_substitution([&](auto const& substitution) -> ErrorOr<void> {
            TRY(Sandbox::add_landlock_path_if_exists(paths, substitution.file_path, Sandbox::LandlockPath::Access::ReadOnly));
            return {};
        }));
    }

    TRY(Sandbox::add_landlock_path_if_exists(paths, cache_path, Sandbox::LandlockPath::Access::ReadWrite));

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_network();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
