/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/WebGLObjectMap.h>

namespace Compositor {

template<typename Value>
static Value lookup_or_default(HashMap<Compositing::WebGL::WebGLObjectId, Value> const& map, Compositing::WebGL::WebGLObjectId id)
{
    return map.get(id).value_or(Value {});
}

template<typename Value>
static Value take_or_default(HashMap<Compositing::WebGL::WebGLObjectId, Value>& map, Compositing::WebGL::WebGLObjectId id)
{
    return map.take(id).value_or(Value {});
}

template<typename Value>
static ErrorOr<void> add_unique(HashMap<Compositing::WebGL::WebGLObjectId, Value>& map, Compositing::WebGL::WebGLObjectId id, Value value)
{
    if (id == 0)
        return Error::from_string_literal("WebGL object id 0 is reserved");
    if (map.contains(id))
        return Error::from_string_literal("WebGL object id is already in use");
    map.set(id, value);
    return {};
}

GLuint WebGLObjectMap::lookup(Compositing::WebGL::WebGLObjectId id) const
{
    return lookup_or_default(m_objects, id);
}

GLuint WebGLObjectMap::take(Compositing::WebGL::WebGLObjectId id)
{
    return take_or_default(m_objects, id);
}

ErrorOr<void> WebGLObjectMap::add(Compositing::WebGL::WebGLObjectId id, GLuint name)
{
    return add_unique(m_objects, id, name);
}

GLsync WebGLObjectMap::lookup_sync(Compositing::WebGL::WebGLObjectId id) const
{
    return lookup_or_default(m_syncs, id);
}

GLsync WebGLObjectMap::take_sync(Compositing::WebGL::WebGLObjectId id)
{
    return take_or_default(m_syncs, id);
}

ErrorOr<void> WebGLObjectMap::add_sync(Compositing::WebGL::WebGLObjectId id, GLsync sync)
{
    return add_unique(m_syncs, id, sync);
}

}
