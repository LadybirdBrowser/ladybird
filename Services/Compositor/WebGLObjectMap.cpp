/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/WebGLObjectMap.h>

namespace Compositor {

template<typename Value>
static Value lookup_or_default(HashMap<Web::WebGL::WebGLObjectId, Value> const& map, Web::WebGL::WebGLObjectId id)
{
    return map.get(id).value_or(Value {});
}

template<typename Value>
static Value take_or_default(HashMap<Web::WebGL::WebGLObjectId, Value>& map, Web::WebGL::WebGLObjectId id)
{
    return map.take(id).value_or(Value {});
}

template<typename Value>
static ErrorOr<void> add_unique(HashMap<Web::WebGL::WebGLObjectId, Value>& map, Web::WebGL::WebGLObjectId id, Value value)
{
    if (id == 0)
        return Error::from_string_literal("WebGL object id 0 is reserved");
    if (map.contains(id))
        return Error::from_string_literal("WebGL object id is already in use");
    map.set(id, value);
    return {};
}

GLuint WebGLObjectMap::lookup(Web::WebGL::WebGLObjectId id) const
{
    return lookup_or_default(m_objects, id);
}

GLuint WebGLObjectMap::take(Web::WebGL::WebGLObjectId id)
{
    return take_or_default(m_objects, id);
}

ErrorOr<void> WebGLObjectMap::add(Web::WebGL::WebGLObjectId id, GLuint name)
{
    return add_unique(m_objects, id, name);
}

GLsync WebGLObjectMap::lookup_sync(Web::WebGL::WebGLObjectId id) const
{
    return lookup_or_default(m_syncs, id);
}

GLsync WebGLObjectMap::take_sync(Web::WebGL::WebGLObjectId id)
{
    return take_or_default(m_syncs, id);
}

ErrorOr<void> WebGLObjectMap::add_sync(Web::WebGL::WebGLObjectId id, GLsync sync)
{
    return add_unique(m_syncs, id, sync);
}

}
