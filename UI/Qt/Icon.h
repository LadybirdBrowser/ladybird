/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

#include <QIcon>

namespace Ladybird {

QIcon load_icon_from_uri(StringView);
QIcon create_tvg_icon_with_theme_colors(QString const& name, QPalette const& palette);

}
