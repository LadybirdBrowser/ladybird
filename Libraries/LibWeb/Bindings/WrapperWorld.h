/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

class WEB_API WrapperWorld final : public GC::Cell {
    GC_CELL(WrapperWorld, GC::Cell);
    GC_DECLARE_ALLOCATOR(WrapperWorld);

public:
    using Type = WrapperWorldType;

    explicit WrapperWorld(Type);
    virtual ~WrapperWorld() override;

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool is_main_world() const { return m_type == Type::Main; }

    [[nodiscard]] GC::Ptr<PlatformObject> wrapper_for(Wrappable const&) const;
    void set_wrapper(Wrappable&, PlatformObject&);
    void clear_wrapper(Wrappable&, PlatformObject const&);

private:
    Type m_type { Type::Main };
    HashMap<Wrappable const*, GC::Weak<PlatformObject>> m_wrappers;
};

WEB_API WrapperWorld& host_defined_wrapper_world(JS::Realm&);
WEB_API WrapperWorld const& host_defined_wrapper_world(JS::Realm const&);

}
