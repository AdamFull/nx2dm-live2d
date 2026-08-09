#pragma once

/**
 * @file live2d_mask.h
 * @brief Where each clipping mask lives in the atlas (namespace nxm::live2d).
 */

#include "live2d/live2d_assets.h"

#include <glm/mat4x4.hpp>

#include <span>

namespace nxm::live2d {

/// One set of drawables rendered together into a tile of the mask atlas.
/// Drawables clipped by the same set share a group; Cubism deduplicates them.
struct MaskGroup {
  u32 atlas = 0;
  u32 channel = 0;
  glm::mat4 to_mask{1.f};
  std::span<const i32> shapes;
};

/// What one clipped drawable needs in order to sample its mask.
struct MaskRef {
  i32 group = -1;
  u32 channel = 0;
  bool inverted = false;
  glm::mat4 to_mask{1.f};

  [[nodiscard]] bool clipped() const noexcept { return group >= 0; }
};

class MaskLayout {
public:
  MaskLayout() = default;
  ~MaskLayout();

  MaskLayout(const MaskLayout &) = delete;
  MaskLayout &operator=(const MaskLayout &) = delete;

  bool build(const ModelAsset &asset, u32 atlas_size = 512,
             u32 atlas_count = 1);

  void update(const ModelAsset &asset);

  [[nodiscard]] std::span<const MaskGroup> groups() const noexcept {
    return {m_groups.data(), m_groups.size()};
  }

  [[nodiscard]] const MaskRef &of(i32 drawable) const noexcept;

  [[nodiscard]] bool active() const noexcept { return !m_groups.empty(); }

  [[nodiscard]] u32 atlas_size() const noexcept { return m_atlas_size; }
  [[nodiscard]] u32 atlas_count() const noexcept { return m_atlas_count; }

private:
  class Clipping;

  Clipping *m_clipping = nullptr;
  nx::vector<MaskGroup> m_groups;
  nx::vector<MaskRef> m_refs;
  nx::vector<nx::vector<i32>> m_shapes;
  u32 m_atlas_size = 512;
  u32 m_atlas_count = 1;
};

} // namespace nxm::live2d
