// Live2D's two passes: coverage into a mask atlas, then the model sampling it.
//
// Cubism clips a drawable against the rendered alpha of other drawables, which
// no amount of arithmetic in the vertex stage can do. Masks pack four to a
// texture across its RGBA channels, so `channel` is a selector with one
// component set and the same vector both writes and reads a mask.

import bindless;
import atlas;
import interop;

// Vectors first, then pointers, then scalars: every member then sits at its
// natural offset and the struct has no padding for the C++ side to guess at.
struct Live2DPush {
    float4 mask_row0;
    float4 mask_row1;

    /// One component set: which of the atlas's channels this mask lives in.
    float4 channel;

    /// Mask pass only: the group's tile as (left, bottom, right, top) in the
    /// clip space mask_row* lands in.
    float4 tile;

    ConstBufferPointer<GpuCamera2D> cameras;
    ConstBufferPointer<GuiVertex> vertices;
    ConstBufferPointer<uint> indices;

    uint index_offset;
    uint vertex_offset;
    uint texture;
    uint camera;
    /// Packed bindless word for the atlas, or kTextureNone when unclipped.
    uint mask_texture;
    /// The mask keeps what it covers, or everything it does not.
    uint inverted;
};

[[vk::push_constant]]
Live2DPush pc;

struct MaskOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    /// Where this vertex landed before the viewport flip, so the fragment can
    /// test it against a tile expressed in Cubism's own clip convention.
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
    // Straight to clip space: a mask is drawn into its own tile of the atlas
    // and never through a camera.
    //
    // Y negated, and it has to be. Cubism's matrix targets an OpenGL clip
    // space where +1 is the top; this engine sets a negative-height viewport,
    // so a mask written at v lands at texel row 1-v while the drawable that
    // uses it samples at v. Without the flip every mask is mirrored inside its
    // own tile, which on a packed atlas means it misses the tile entirely.
    const float2 clip = nx_l2d_affine(vertex.position);
    output.position = float4(clip.x, -clip.y, 0.0, 1.0);
    output.uv = vertex.uv;
    output.clip = clip;
    return output;
}

[shader("fragment")]
float4 mask_fs(MaskOutput input): SV_Target {
    // Clamped to the group's own tile. A mask shape is fitted to the art it
    // clips, not to itself, so a large one overhangs - and its neighbour in the
    // same channel is right there. Cubism's own mask shader does the same four
    // step tests for the same reason.
    const float inside = step(pc.tile.x, input.clip.x) *
                         step(pc.tile.y, input.clip.y) *
                         step(input.clip.x, pc.tile.z) *
                         step(input.clip.y, pc.tile.w);

    // The artwork's alpha, not the shape's silhouette: a mask painted with a
    // soft edge clips with a soft edge. Deliberately not the drawable's
    // opacity - mask shapes are routinely invisible helpers.
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

    // Saturated because the mask pass adds: overlapping shapes in one group
    // can push a channel past one, and coverage above full is still full.
    float coverage = saturate(dot(nx_sample_packed(pc.mask_texture, input.mask_uv),
                                  pc.channel));
    // A drawable is clipped to what its mask covers, or to what it does not.
    if (pc.inverted != 0u)
        coverage = 1.0 - coverage;
    // Alpha only. Scaling the colour as well would darken the surviving pixels
    // towards black at the mask's soft edge rather than fading them out.
    result.a *= coverage;
    return result;
}
