/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibGfx/ICC/DistinctFourCC.h>

namespace Gfx::ICC {

// ICC v4, 9.2 Tag listing
#define ENUMERATE_TAG_SIGNATURES(TAG)                               \
    TAG(AToB0Tag, 0x41324230 /* 'A2B0' */)                          \
    TAG(AToB1Tag, 0x41324231 /* 'A2B1' */)                          \
    TAG(AToB2Tag, 0x41324232 /* 'A2B2' */)                          \
    TAG(blueMatrixColumnTag, 0x6258595A /* 'bXYZ' */)               \
    TAG(blueTRCTag, 0x62545243 /* 'bTRC' */)                        \
    TAG(BToA0Tag, 0x42324130 /* 'B2A0' */)                          \
    TAG(BToA1Tag, 0x42324131 /* 'B2A1' */)                          \
    TAG(BToA2Tag, 0x42324132 /* 'B2A2' */)                          \
    TAG(BToD0Tag, 0x42324430 /* 'B2D0' */)                          \
    TAG(BToD1Tag, 0x42324431 /* 'B2D1' */)                          \
    TAG(BToD2Tag, 0x42324432 /* 'B2D2' */)                          \
    TAG(BToD3Tag, 0x42324433 /* 'B2D3' */)                          \
    TAG(calibrationDateTimeTag, 0x63616C74 /* 'calt' */)            \
    TAG(charTargetTag, 0x74617267 /* 'targ' */)                     \
    TAG(chromaticAdaptationTag, 0x63686164 /* 'chad' */)            \
    TAG(chromaticityTag, 0x6368726D /* 'chrm' */)                   \
    TAG(cicpTag, 0x63696370 /* 'cicp' */)                           \
    TAG(colorantOrderTag, 0x636C726F /* 'clro' */)                  \
    TAG(colorantTableTag, 0x636C7274 /* 'clrt' */)                  \
    TAG(colorantTableOutTag, 0x636C6F74 /* 'clot' */)               \
    TAG(colorimetricIntentImageStateTag, 0x63696973 /* 'ciis' */)   \
    TAG(copyrightTag, 0x63707274 /* 'cprt' */)                      \
    TAG(deviceMfgDescTag, 0x646D6E64 /* 'dmnd' */)                  \
    TAG(deviceModelDescTag, 0x646D6464 /* 'dmdd' */)                \
    TAG(DToB0Tag, 0x44324230 /* 'D2B0' */)                          \
    TAG(DToB1Tag, 0x44324231 /* 'D2B1' */)                          \
    TAG(DToB2Tag, 0x44324232 /* 'D2B2' */)                          \
    TAG(DToB3Tag, 0x44324233 /* 'D2B3' */)                          \
    TAG(gamutTag, 0x67616D74 /* 'gamt' */)                          \
    TAG(grayTRCTag, 0x6B545243 /* 'kTRC' */)                        \
    TAG(greenMatrixColumnTag, 0x6758595A /* 'gXYZ' */)              \
    TAG(greenTRCTag, 0x67545243 /* 'gTRC' */)                       \
    TAG(luminanceTag, 0x6C756D69 /* 'lumi' */)                      \
    TAG(measurementTag, 0x6D656173 /* 'meas' */)                    \
    TAG(metadataTag, 0x6D657461 /* 'meta' */)                       \
    TAG(mediaWhitePointTag, 0x77747074 /* 'wtpt' */)                \
    TAG(namedColor2Tag, 0x6E636C32 /* 'ncl2' */)                    \
    TAG(outputResponseTag, 0x72657370 /* 'resp' */)                 \
    TAG(perceptualRenderingIntentGamutTag, 0x72696730 /* 'rig0' */) \
    TAG(preview0Tag, 0x70726530 /* 'pre0' */)                       \
    TAG(preview1Tag, 0x70726531 /* 'pre1' */)                       \
    TAG(preview2Tag, 0x70726532 /* 'pre2' */)                       \
    TAG(profileDescriptionTag, 0x64657363 /* 'desc' */)             \
    TAG(profileSequenceDescTag, 0x70736571 /* 'pseq' */)            \
    TAG(profileSequenceIdentifierTag, 0x70736964 /* 'psid' */)      \
    TAG(redMatrixColumnTag, 0x7258595A /* 'rXYZ' */)                \
    TAG(redTRCTag, 0x72545243 /* 'rTRC' */)                         \
    TAG(saturationRenderingIntentGamutTag, 0x72696732 /* 'rig2' */) \
    TAG(technologyTag, 0x74656368 /* 'tech' */)                     \
    TAG(viewingCondDescTag, 0x76756564 /* 'vued' */)                \
    TAG(viewingConditionsTag, 0x76696577 /* 'view' */)              \
    /* The following tags are v2-only */                            \
    TAG(crdInfoTag, 0x63726469 /* 'crdi' */)                        \
    TAG(deviceSettingsTag, 0x64657673 /* 'devs' */)                 \
    TAG(mediaBlackPointTag, 0x626B7074 /* 'bkpt' */)                \
    TAG(namedColorTag, 0x6E636F6C /* 'ncol' */)                     \
    TAG(ps2CRD0Tag, 0x70736430 /* 'psd0' */)                        \
    TAG(ps2CRD1Tag, 0x70736431 /* 'psd1' */)                        \
    TAG(ps2CRD2Tag, 0x70736432 /* 'psd2' */)                        \
    TAG(ps2CRD3Tag, 0x70736433 /* 'psd3' */)                        \
    TAG(ps2CSATag, 0x70733273 /* 'ps2s' */)                         \
    TAG(ps2RenderingIntentTag, 0x70733269 /* 'ps2i' */)             \
    TAG(screeningDescTag, 0x73637264 /* 'scrd' */)                  \
    TAG(screeningTag, 0x7363726E /* 'scrn' */)                      \
    TAG(ucrbgTag, 0x62666420 /* 'bfd ' */)

#define TAG(name, id) constexpr inline TagSignature name { id };
ENUMERATE_TAG_SIGNATURES(TAG)
#undef TAG

Optional<StringView> tag_signature_spec_name(TagSignature);

}
