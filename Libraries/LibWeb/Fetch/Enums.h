/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

[[nodiscard]] ReferrerPolicy::ReferrerPolicy from_bindings_enum(Bindings::ReferrerPolicy);
[[nodiscard]] Infrastructure::Request::Mode from_bindings_enum(Bindings::RequestMode);
[[nodiscard]] Infrastructure::Request::CredentialsMode from_bindings_enum(Bindings::RequestCredentials);
[[nodiscard]] Infrastructure::Request::CacheMode from_bindings_enum(Bindings::RequestCache);
[[nodiscard]] Infrastructure::Request::RedirectMode from_bindings_enum(Bindings::RequestRedirect);
[[nodiscard]] Infrastructure::Request::Priority from_bindings_enum(Bindings::RequestPriority);

[[nodiscard]] Bindings::ReferrerPolicy to_bindings_enum(ReferrerPolicy::ReferrerPolicy);
[[nodiscard]] Bindings::RequestDestination to_bindings_enum(Optional<Infrastructure::Request::Destination> const&);
[[nodiscard]] Bindings::RequestMode to_bindings_enum(Infrastructure::Request::Mode);
[[nodiscard]] Bindings::RequestCredentials to_bindings_enum(Infrastructure::Request::CredentialsMode);
[[nodiscard]] Bindings::RequestCache to_bindings_enum(Infrastructure::Request::CacheMode);
[[nodiscard]] Bindings::RequestRedirect to_bindings_enum(Infrastructure::Request::RedirectMode);
[[nodiscard]] Bindings::ResponseType to_bindings_enum(Infrastructure::Response::Type);

}
