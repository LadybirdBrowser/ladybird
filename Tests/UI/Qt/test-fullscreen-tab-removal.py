#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import sys

from pathlib import Path

source = Path(sys.argv[1]).read_text()
start = source.index("void BrowserWindow::exit_fullscreen()")
end = source.index("bool BrowserWindow::event(QEvent* event)", start)
exit_fullscreen = source[start:end]

guard = exit_fullscreen.find("if (!current_tab())")
first_chrome_update = exit_fullscreen.index("m_tabs_container->set_tab_bar_visible")
assert 0 <= guard < first_chrome_update, "final-tab removal must stop fullscreen chrome updates"
