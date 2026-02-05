/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FileRequest.h"

namespace Web {

FileRequest::FileRequest(ByteString path, Function<void(ErrorOr<i32>)> on_file_request_finish_callback)
    : on_file_request_finish(move(on_file_request_finish_callback))
    , m_path(move(path))
{
}

ByteString FileRequest::path() const
{
    return m_path;
}

}
