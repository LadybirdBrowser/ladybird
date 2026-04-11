/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CaptureFile.h"
#include "Application.h"
#include "Debug.h"

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>

namespace TestWeb {

CaptureFile::CaptureFile(ByteString destination_path)
    : m_destination_path(move(destination_path))
{
    ByteString relative_destination_path = LexicalPath::relative_path(m_destination_path, Application::the().results_directory).value_or(m_destination_path);
    StringBuilder builder;
    for (char ch : relative_destination_path)
        builder.append((ch == '/' || ch == '\\') ? '_' : ch);
    builder.append(".capture"sv);
    m_writer_path = LexicalPath::join(Application::the().results_directory, builder.string_view()).string();
    if (auto maybe_writer = Core::File::open(m_writer_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !maybe_writer.is_error())
        m_writer = maybe_writer.release_value();
}

ErrorOr<bool> CaptureFile::transfer_to_output_file()
{
    bool has_content = m_writer != nullptr && m_writer->tell().value_or(0) > 0;
    m_writer = nullptr;
    if (m_writer_path.is_empty())
        return false;

    ScopeGuard cleanup = [&] {
        (void)Core::System::unlink(m_writer_path);
        m_writer_path = {};
        m_destination_path = {};
    };
    if (!has_content)
        return false;

    auto raw_capture = TRY(Core::File::open(m_writer_path, Core::File::OpenMode::Read));
    auto converted = convert_ansi_to_html(TRY(raw_capture->read_until_eof()));
    TRY(Core::Directory::create(LexicalPath(m_destination_path).dirname(), Core::Directory::CreateDirectories::Yes));
    auto html_file = TRY(Core::File::open(m_destination_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(html_file->write_until_depleted(converted.string_view().bytes()));
    return true;
}

void CaptureFile::write(StringView message)
{
    if (m_writer && !message.is_empty())
        MUST(m_writer->write_until_depleted(message.bytes()));
}

}
