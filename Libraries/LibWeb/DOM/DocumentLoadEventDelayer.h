/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class DocumentLoadEventDelayer {
    AK_MAKE_NONCOPYABLE(DocumentLoadEventDelayer);

public:
    explicit DocumentLoadEventDelayer(Document&);

    DocumentLoadEventDelayer(DocumentLoadEventDelayer&&);
    DocumentLoadEventDelayer& operator=(DocumentLoadEventDelayer&&);

    ~DocumentLoadEventDelayer();

private:
    GC::Weak<Document> m_document;
};

}
