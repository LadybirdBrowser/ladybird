/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/CSS/StyleSheetIdentifier.h>

namespace Web::CSS {

WEB_API Optional<StyleSheetIdentifier> style_sheet_identifier_for(StyleSheetState const&);

}
