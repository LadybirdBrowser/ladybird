/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <LibCore/File.h>

namespace TestWeb {

class CaptureFile {
public:
    CaptureFile() = default;
    explicit CaptureFile(ByteString);

    void write(StringView);
    ErrorOr<bool> transfer_to_output_file();

private:
    ByteString m_destination_path;
    ByteString m_writer_path;
    OwnPtr<Core::File> m_writer;
};

}
