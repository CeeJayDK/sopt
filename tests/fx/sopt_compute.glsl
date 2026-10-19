// sopt test: a Vulkan GLSL compute shader as FFmpeg writes them (libavfilter/vulkan): loads, and its math is regions.
#pragma shader_stage(compute)
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_image_load_formatted : require
#extension GL_EXT_null_initializer : require

layout (local_size_x_id = 253, local_size_y_id = 254, local_size_z_id = 255) in;
layout (constant_id = 0) const float strength = 0;

layout (set = 0, binding = 0) uniform sampler2D input_img[];
layout (set = 0, binding = 1) uniform writeonly image2D output_img[];
layout (push_constant, scalar) uniform pushConstants {
    mat3 yuv_matrix;
    vec2 dist;
};
layout (set = 1, binding = 0) readonly buffer Weights { float weights[]; } wbuf;
shared vec4 cache[64] = { };
ivec2 sizes[2] = { ivec2(gl_NumWorkGroups.xy), ivec2(gl_WorkGroupSize.xy) };
const float taps[3] = { 0.25, 0.5, 0.25 };

vec4 tap(vec2 p)
{
    nonuniformEXT uint idx = gl_WorkGroupID.z;
    return texture(input_img[idx], p * 0.5 + 0.5);
}

void main()
{
    const ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(output_img[nonuniformEXT(gl_WorkGroupID.z)]);
    if (pos == size)
        return;
    vec2 p = (vec2(pos) / vec2(size) - 0.5) * 2.0;
    vec3 c = yuv_matrix * tap(p * dist).rgb;
    c *= yuv_matrix;
    float w = 0.0;
    for (int i = 0; i < taps.length(); i++)
        w += taps[i] * wbuf.weights[i];
    float s = p.y * strength + p.y * 2.0 - p.y;
    cache[gl_LocalInvocationIndex] = vec4(c, w);
    barrier();
    memoryBarrierShared();
    imageStore(output_img[gl_WorkGroupID.z], pos, vec4(cache[0].rgb * s, 1.0));
}
