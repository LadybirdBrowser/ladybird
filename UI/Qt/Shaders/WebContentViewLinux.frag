/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#version 450

layout(set = 0, binding = 0) uniform sampler2D web_content_texture;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 output_color;

void main()
{
    output_color = texture(web_content_texture, texture_coordinates);
}
