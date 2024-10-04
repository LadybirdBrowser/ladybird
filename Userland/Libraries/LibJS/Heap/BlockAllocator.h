/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Forward.h>

namespace JS {

class BlockAllocator {
public:
    BlockAllocator() = default;
    ~BlockAllocator();

    void* allocate_block(char const* name);
    void deallocate_block(void*);

private:
    Vector<void*> m_blocks;
};

}
