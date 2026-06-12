/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>

namespace Gfx {

class ExifMetadata;

namespace TIFF {

// Exif data is encoded as a TIFF structure: a set of TIFF tags, without any associated image data.
//
// This is a link to the main TIFF specification from 1992
// https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf
//
// The Exif specification is named "Exchangeable image file format for digital still cameras: Exif Version 3.0"
// and can be found at https://www.cipa.jp/e/std/std-sec.html
ErrorOr<NonnullOwnPtr<ExifMetadata>> read_exif_metadata(ReadonlyBytes);

}

}
