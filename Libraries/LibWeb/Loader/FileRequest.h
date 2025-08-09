/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Swift.h>
#include <LibWeb/Export.h>

namespace Web {

class WEB_API FileRequest {
public:
    FileRequest(ByteString path, ESCAPING Function<void(ErrorOr<i32>)> on_file_request_finish);

    ByteString path() const;

    Function<void(ErrorOr<i32>)> on_file_request_finish;

private:
    ByteString m_path {};
} SWIFT_UNSAFE_REFERENCE; // FIXME: This type is actually move-only, not unsafe

}
