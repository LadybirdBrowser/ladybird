/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Loader/Resource.h>

namespace Web {

static String s_chrome_process_command_line {};
static String s_chrome_process_executable_path {};

void set_chrome_process_command_line(StringView command_line);
void set_chrome_process_executable_path(StringView executable_path);

ErrorOr<String> load_error_page(URL::URL const&, StringView error_message);

ErrorOr<String> load_file_directory_page(URL::URL const&);

ErrorOr<String> load_about_version_page();

}
