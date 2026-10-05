#pragma once

// What the Live2D draws are handed, shared by C++ and Slang as nx_interop.h
// describes; live2d.slang includes it and imports interop for GpuCamera2D.

#include "rendering/rhi/shaders/nx_interop.h"

#ifdef __cplusplus
#include "rendering/interop/2d/scene.h"
#endif

NX_CONST(uint, NX_L2D_MASK_ATLASES, 16u);
NX_CONST(uint, NX_L2D_NO_MASK, 0xFFFFFFFFu);

/// One drawable or mask shape. Positions are in model space: a model draw
/// reads row0/row1 as its model-to-atlas affine and world0/world1 as its
/// model-to-world one; a mask draw reads row0/row1 as model-to-clip and clamps
/// to tile. uvs and indices are the model's static mesh, uploaded once.
struct GpuLive2DDraw {
  float4 row0 = float4(0.f, 0.f, 0.f, 0.f);
  float4 row1 = float4(0.f, 0.f, 0.f, 0.f);
  float4 world0 = float4(1.f, 0.f, 0.f, 0.f);
  float4 world1 = float4(0.f, 1.f, 0.f, 0.f);
  float4 channel = float4(0.f, 0.f, 0.f, 0.f);
  float4 tile = float4(-1.f, -1.f, 1.f, 1.f);
  NxPtr<float2> uvs = {};
  NxPtr<uint> indices = {};
  uint first_index = 0u;
  uint first_vertex = 0u;
  uint positions = 0u;
  NxTexture2D<float4> texture = {};
  uint camera = 0u;
  uint mask = NX_L2D_NO_MASK;
  uint inverted = 0u;
  uint color = 0u;
};
NX_SHARED_SIZE(GpuLive2DDraw, 144);

struct GpuLive2DPush {
  NxPtr<GpuCamera2D> cameras = {};
  NxPtr<float2> positions = {};
  NxPtr<GpuLive2DDraw> draws = {};
  NxTexture2D<float4> masks[NX_L2D_MASK_ATLASES] = {};
};
NX_SHARED_SIZE(GpuLive2DPush, 88);
