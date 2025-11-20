/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#null-body-status
bool is_null_body_status(Status status) {
  // A null body status is a status that is 101, 103, 204, 205, or 304.
  return any_of(
      Array<Status, 5>{101, 103, 204, 205, 304},
      [&](auto redirect_status) { return status == redirect_status; });
}

// https://fetch.spec.whatwg.org/#ok-status
bool is_ok_status(Status status) {
  // An ok status is a status in the range 200 to 299, inclusive.
  return status >= 200 && status <= 299;
}

// https://fetch.spec.whatwg.org/#redirect-status
bool is_redirect_status(Status status) {
  // A redirect status is a status that is 301, 302, 303, 307, or 308.
  return any_of(
      Array<Status, 5>{301, 302, 303, 307, 308},
      [&](auto redirect_status) { return status == redirect_status; });
}

// https://httpwg.org/specs/rfc9110.html#rfc.section.15.1
bool is_heuristically_cacheable_status(Status status) {
  return any_of(
      Array<Status, 5>{200, 203, 204, 206, 300, 301, 308, 404, 405, 410, 414,
                       501},
      [&](auto cacheable_status) { return status == cacheable_status; });
}

}
