/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibGC/Handle.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class DocumentLoadEventDelayer {
    AK_MAKE_NONMOVABLE(DocumentLoadEventDelayer);
    AK_MAKE_NONCOPYABLE(DocumentLoadEventDelayer);

public:
    explicit DocumentLoadEventDelayer(Document&);
    ~DocumentLoadEventDelayer();

private:
    GC::Handle<Document> m_document;
};

}
