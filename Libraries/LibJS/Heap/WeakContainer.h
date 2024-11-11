/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <LibJS/Forward.h>

namespace JS {

class WeakContainer {
public:
    explicit WeakContainer(Heap&);
    virtual ~WeakContainer();

    virtual void remove_dead_cells(Badge<Heap>) = 0;

protected:
    void deregister();

private:
    bool m_registered { true };
    Heap& m_heap;

    IntrusiveListNode<WeakContainer> m_list_node;

public:
    using List = IntrusiveList<&WeakContainer::m_list_node>;
};

}
