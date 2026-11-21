/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibGfx/ExportFormat.h>

namespace Compositing::WebGL {

COMPOSITING_API Optional<Gfx::ExportFormat> texture_export_format(GLenum format, GLenum type);

}
