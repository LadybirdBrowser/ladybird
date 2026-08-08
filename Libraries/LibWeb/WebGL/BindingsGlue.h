/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/WebGLContextAttributes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

class WebGL2RenderingContext;
class WebGLExtension;
class WebGLProgram;
class WebGLQuery;
class WebGLRenderingContext;
class WebGLRenderingContextBase;
class WebGLRenderingContextImpl;
class WebGLShader;
class WebGLSync;
class WebGLUniformLocation;

}

namespace Web::Bindings {

class Wrappable;

WEB_API JS::Object* webgl_extension(JS::Realm&, GC::Ref<WebGL::WebGLExtension>);
WEB_API JS::Value webgl_wrappable(JS::Realm&, GC::Ref<Wrappable>);
WEB_API JS::Value webgl_buffer(JS::Realm&, GC::Ref<WebGL::WebGLRenderingContextBase>, u32 handle);
WEB_API Optional<WebGLContextAttributes> get_context_attributes(JS::Realm&, WebGL::WebGLRenderingContext&);
WEB_API Optional<WebGLContextAttributes> get_context_attributes(JS::Realm&, WebGL::WebGL2RenderingContext&);
WEB_API JS::Object* get_extension(JS::Realm&, WebGL::WebGLRenderingContext&, String const& name);
WEB_API JS::Object* get_extension(JS::Realm&, WebGL::WebGL2RenderingContext&, String const& name);
WEB_API JS::Value get_buffer_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_program_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, GC::Ref<WebGL::WebGLProgram>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_renderbuffer_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_shader_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, GC::Ref<WebGL::WebGLShader>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_tex_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_vertex_attrib(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_uniform(JS::Realm&, WebGL::WebGLRenderingContextImpl&, GC::Ref<WebGL::WebGLProgram>, GC::Ref<WebGL::WebGLUniformLocation>);
WEB_API JS::Value get_query_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLQuery>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_sync_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLSync>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_active_uniforms(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLProgram>, Vector<WebIDL::UnsignedLong> uniform_indices, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_internalformat_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_active_uniform_block_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLProgram>, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong pname);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> make_xr_compatible(JS::Realm&, WebGL::WebGLRenderingContextBase&);

template<typename T>
JS::Value webgl_wrappable(JS::Realm& realm, GC::Ptr<T> wrappable)
{
    if (!wrappable)
        return JS::js_null();
    return webgl_wrappable(realm, GC::Ref<Wrappable> { *wrappable });
}

}
