import nx_core;
import atlas;
import interop;

static const uint NX_L2D_MASK_ATLASES = 16;

// Mirrors DrawRecord in live2d_pass.h. Positions are in model space; uvs and
// indices are the model's static mesh, uploaded once.
struct Live2DDraw {
    float4 row0;
    float4 row1;
    float4 world0;
    float4 world1;
    float4 channel;
    float4 tile;
    NxPtr<float2> uvs;
    NxPtr<uint> indices;
    uint first_index;
    uint first_vertex;
    uint positions;
    NxTexture2D<float4> texture;
    uint camera;
    uint mask;
    uint inverted;
    uint color;
};

struct Live2DPush {
    NxPtr<GpuCamera2D> cameras;
    NxPtr<float2> positions;
    NxPtr<Live2DDraw> draws;
    NxTexture2D<float4> masks[NX_L2D_MASK_ATLASES];
};

[[vk::push_constant]]
Live2DPush pc;

struct MaskOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 clip : TEXCOORD1;
    nointerpolation float4 channel : TEXCOORD2;
    nointerpolation float4 tile : TEXCOORD3;
    nointerpolation NxTexture2D<float4> texture : TEXCOORD4;
};

struct ModelOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float2 mask_uv : TEXCOORD1;
    nointerpolation float4 channel : TEXCOORD2;
    nointerpolation NxTexture2D<float4> texture : TEXCOORD3;
    nointerpolation uint mask : TEXCOORD4;
    nointerpolation uint inverted : TEXCOORD5;
};

float2 nx_l2d_affine(float4 row0, float4 row1, float2 p) {
    return float2(dot(row0.xy, p) + row0.z, dot(row1.xy, p) + row1.z);
}

// The drawable's vertex_id-th corner: its model-space position and uv.
void nx_l2d_vertex(Live2DDraw record, uint vertex_id, out float2 position,
                   out float2 uv) {
    const uint vertex =
        record.first_vertex + record.indices[record.first_index + vertex_id];
    position = pc.positions[record.positions + vertex];
    uv = record.uvs[vertex];
}

[shader("vertex")]
MaskOutput mask_vs(uint vertex_id: SV_VertexID,
                   uint instance_id: SV_InstanceID,
                   uint start_instance: SV_StartInstanceLocation) {
    const Live2DDraw record = pc.draws[start_instance + instance_id];
    float2 position;
    float2 uv;
    nx_l2d_vertex(record, vertex_id, position, uv);

    MaskOutput output;
    const float2 clip = nx_l2d_affine(record.row0, record.row1, position);
    output.position = float4(clip.x, -clip.y, 0.0, 1.0);
    output.uv = uv;
    output.clip = clip;
    output.channel = record.channel;
    output.tile = record.tile;
    output.texture = record.texture;
    return output;
}

[shader("fragment")]
float4 mask_fs(MaskOutput input): SV_Target {
    const float inside = step(input.tile.x, input.clip.x) *
                         step(input.tile.y, input.clip.y) *
                         step(input.clip.x, input.tile.z) *
                         step(input.clip.y, input.tile.w);

    const float coverage = input.texture.sample_color(input.uv).a * inside;

    // Additive, so two shapes in one group union rather than the later one
    // replacing the earlier. Channels do not interfere because only one
    // component of the selector is set.
    return coverage * input.channel;
}

[shader("vertex")]
ModelOutput model_vs(uint vertex_id: SV_VertexID,
                     uint instance_id: SV_InstanceID,
                     uint start_instance: SV_StartInstanceLocation) {
    const Live2DDraw record = pc.draws[start_instance + instance_id];
    float2 position;
    float2 uv;
    nx_l2d_vertex(record, vertex_id, position, uv);
    const GpuCamera2D cam = pc.cameras[record.camera];
    const float2 world = nx_l2d_affine(record.world0, record.world1, position);

    ModelOutput output;
    output.position = mul(cam.view_proj, float4(world, 0.0, 1.0));
    output.color = nx_unpack_rgba8(record.color);
    output.uv = uv;
    output.mask_uv = nx_l2d_affine(record.row0, record.row1, position);
    output.channel = record.channel;
    output.texture = record.texture;
    output.mask = record.mask;
    output.inverted = record.inverted;
    return output;
}

[shader("fragment")]
float4 model_fs(ModelOutput input): SV_Target {
    float4 result = input.color * input.texture.sample_color(input.uv);
    if (input.mask >= NX_L2D_MASK_ATLASES)
        return result;
    const NxTexture2D<float4> mask = pc.masks[input.mask];
    if (!mask.is_valid())
        return result;

    float coverage =
        saturate(dot(mask.sample_color(input.mask_uv), input.channel));
    if (input.inverted != 0u)
        coverage = 1.0 - coverage;
    result.a *= coverage;
    return result;
}
