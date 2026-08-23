
import bindless;
import atlas;
import interop;

struct Live2DPush {
    float4 mask_row0;
    float4 mask_row1;

    float4 channel;

    float4 tile;

    ConstBufferPointer<GpuCamera2D> cameras;
    ConstBufferPointer<GuiVertex> vertices;
    ConstBufferPointer<uint> indices;

    uint index_offset;
    uint vertex_offset;
    uint texture;
    uint camera;
    uint mask_texture;
    uint inverted;
};

[[vk::push_constant]]
Live2DPush pc;

struct MaskOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 clip : TEXCOORD1;
};

struct ModelOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float2 mask_uv : TEXCOORD1;
};

float2 nx_l2d_affine(float2 p) {
    return float2(dot(pc.mask_row0.xy, p) + pc.mask_row0.z,
                  dot(pc.mask_row1.xy, p) + pc.mask_row1.z);
}

[shader("vertex")]
MaskOutput mask_vs(uint vertex_id: SV_VertexID) {
    const uint index = pc.indices[pc.index_offset + vertex_id];
    const GuiVertex vertex = pc.vertices[pc.vertex_offset + index];

    MaskOutput output;
    const float2 clip = nx_l2d_affine(vertex.position);
    output.position = float4(clip.x, -clip.y, 0.0, 1.0);
    output.uv = vertex.uv;
    output.clip = clip;
    return output;
}

[shader("fragment")]
float4 mask_fs(MaskOutput input): SV_Target {
    const float inside = step(pc.tile.x, input.clip.x) *
                         step(pc.tile.y, input.clip.y) *
                         step(input.clip.x, pc.tile.z) *
                         step(input.clip.y, pc.tile.w);

    const float coverage = nx_sample_packed(pc.texture, input.uv).a * inside;

    // Additive, so two shapes in one group union rather than the later one
    // replacing the earlier. Channels do not interfere because only one
    // component of the selector is set.
    return coverage * pc.channel;
}

[shader("vertex")]
ModelOutput model_vs(uint vertex_id: SV_VertexID) {
    const uint index = pc.indices[pc.index_offset + vertex_id];
    const GuiVertex vertex = pc.vertices[pc.vertex_offset + index];
    const GpuCamera2D cam = pc.cameras[pc.camera];

    ModelOutput output;
    output.position = mul(cam.view_proj, float4(vertex.position, 0.0, 1.0));
    output.color = nx_unpack_rgba8(vertex.color);
    output.uv = vertex.uv;
    output.mask_uv = nx_l2d_affine(vertex.position);
    return output;
}

[shader("fragment")]
float4 model_fs(ModelOutput input): SV_Target {
    float4 result = input.color * nx_sample_packed(pc.texture, input.uv);
    if ((pc.mask_texture >> 16) == kTextureNone)
        return result;

    float coverage = saturate(dot(nx_sample_packed(pc.mask_texture, input.mask_uv),
                                  pc.channel));
    if (pc.inverted != 0u)
        coverage = 1.0 - coverage;
    result.a *= coverage;
    return result;
}
