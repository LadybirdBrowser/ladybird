/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Tests/LibMedia/TestMediaCommon.h>

TEST_CASE(xiph_lacing)
{
    // FIXME: 530 samples are marked to be discarded by the DiscardPadding element.
    decode_audio("test-webm-xiph-lacing.mka"sv, 48'000, 2, 578038 + 530);
}
