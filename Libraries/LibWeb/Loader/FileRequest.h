/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>

namespace Web {

class WEB_API FileRequest {
public:
    FileRequest(ByteString path, Optional<HTML::EnvironmentId> environment_id, ESCAPING Function<void(ErrorOr<i32>)> on_file_request_finish);

    ByteString path() const;
    Optional<HTML::EnvironmentId> const& environment_id() const { return m_environment_id; }

    Function<void(ErrorOr<i32>)> on_file_request_finish;

private:
    ByteString m_path {};
    Optional<HTML::EnvironmentId> m_environment_id;
};

}
