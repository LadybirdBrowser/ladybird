/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Forward.h>
#include <LibWeb/Forward.h>

namespace Web {

enum class MentionCertificateSetting : u8 {
    No,
    SelfSigned,
    Invalid,
};
ErrorOr<String> load_error_page(URL::URL const&, StringView error_message, MentionCertificateSetting = MentionCertificateSetting::No);
ErrorOr<String> load_file_directory_page(URL::URL const&);

}
