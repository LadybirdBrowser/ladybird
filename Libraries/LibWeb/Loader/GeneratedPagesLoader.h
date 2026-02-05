/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Forward.h>
#include <LibWeb/Forward.h>

namespace Web {

WEB_API void set_browser_process_command_line(StringView command_line);
WEB_API void set_browser_process_executable_path(StringView executable_path);

ErrorOr<String> load_error_page(URL::URL const&, StringView error_message);

ErrorOr<String> load_file_directory_page(URL::URL const&);

ErrorOr<String> load_about_version_page();

}
