/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, circl <circl.lastname@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "QtUtils.h"

namespace Ladybird {

bool is_using_dark_system_theme(QWidget& widget)
{
    // FIXME: Qt does not provide any method to query if the system is using a dark theme. We will have to implement
    //        platform-specific methods if we wish to have better detection. For now, this inspects if Qt is using a
    //        dark color for widget backgrounds using Rec. 709 luma coefficients.
    //        https://en.wikipedia.org/wiki/Rec._709#Luma_coefficients

    auto color = widget.palette().color(widget.backgroundRole());
    auto luma = 0.2126f * color.redF() + 0.7152f * color.greenF() + 0.0722f * color.blueF();

    return luma <= 0.5f;
}

}
