/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace WebView {

// FIXME: Move these to an HTML file in Base/res/ladybird.

constexpr inline auto ERROR_HTML_HEADER = R"~~~(
<!doctype html>
<html lang="en">
    <head>
        <meta charset="UTF-8" />
        <title>Error!</title>
        <style>
            :root {{
                color-scheme: light dark;
                font-family: system-ui, sans-serif;
            }}
            body {{
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: center;
                min-height: 100vh;
                box-sizing: border-box;
                margin: 0;
                padding: 1rem;
                text-align: center;
            }}
            header {{
                display: flex;
                flex-direction: column;
                align-items: center;
                gap: 2rem;
                margin-bottom: 1rem;
            }}
            svg {{
                height: 64px;
                width: auto;
                stroke: currentColor;
                fill: none;
                stroke-width: 1.5;
                stroke-linecap: round;
                stroke-linejoin: round;
            }}
            h1 {{
                margin: 0;
                font-size: 1.5rem;
            }}
            p {{
                font-size: 1rem;
                color: #555;
            }}
        </style>
    </head>
    <body>
        <header>
            {}
            <h1>{}</h1>
        </header>
)~~~"sv;

constexpr inline auto ERROR_HTML_FOOTER = R"~~~(
    </body>
    </html>
)~~~"sv;

constexpr inline auto ERROR_SVG = R"~~~(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 17.5 21.5">
        <path d="M11.75.75h-9c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2v-13l-5-5z" />
        <path d="M10.75.75v4c0 1.1.9 2 2 2h4M5.75 9.75v2M11.75 9.75v2M5.75 16.75c1-2.67 5-2.67 6 0" />
    </svg>
)~~~"sv;

constexpr inline auto CRASH_ERROR_SVG = R"~~~(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 17.5 21.5">
        <path class="b" d="M11.75.75h-9c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2v-13l-5-5z" />
        <path class="b" d="M10.75.75v4c0 1.1.9 2 2 2h4M4.75 9.75l2 2M10.75 9.75l2 2M12.75 9.75l-2 2M6.75 9.75l-2 2M5.75 16.75c1-2.67 5-2.67 6 0" />
    </svg>
)~~~"sv;

}
