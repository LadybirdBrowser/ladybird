/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSyntax/Highlighter.h>

namespace Syntax {

void Highlighter::attach(HighlighterClient& client)
{
    VERIFY(!m_client);
    m_client = &client;
}

void Highlighter::detach()
{
    m_client = nullptr;
}

}
