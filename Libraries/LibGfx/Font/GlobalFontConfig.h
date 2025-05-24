/*
 * Copyright (c) 2025, blukai <init1@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <fontconfig/fontconfig.h>

namespace Gfx {

class GlobalFontConfig {
public:
    static GlobalFontConfig& the();
    FcConfig* get();

private:
    GlobalFontConfig();
    ~GlobalFontConfig();

    GlobalFontConfig(GlobalFontConfig const&) = delete;
    GlobalFontConfig& operator=(GlobalFontConfig const&) = delete;

    FcConfig* m_config;
};

}
