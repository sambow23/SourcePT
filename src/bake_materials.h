#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace proxy::bsp {

// A group's backdrop role where its type has one (the map's groups), else 0.
template <class Group> auto backdropRole(const Group &group, int) -> decltype(int(group.backdrop)) {
  return int(group.backdrop);
}
template <class Group> int backdropRole(const Group &, long) { return 0; }

// BSP material tables also contain unused faces and engine-rendered brush
// models. Only geometry retained by our map submission creates dependencies.
template <class World> std::vector<uint32_t> bakeMaterialDependencies(const World &world) {
  std::vector<uint32_t> required;
  auto groups = [&](const auto &collection) {
    for (const auto &[key, group] : collection) {
      if (group.sky)
        continue; // excluded from the retained bake instance list
      // A backdrop portal (the map's sky faces): no ray meets it, and its
      // material (the sky brush's) is never drawn by the engine.
      if (backdropRole(group, 0) == 3)
        continue;
      for (const auto &[material, geometry] : group.surfaces)
        if (!geometry.vertices.empty() && !geometry.indices.empty())
          required.push_back(material);
    }
  };
  groups(world.groups);
  groups(world.overlayGroups);
  groups(world.layerGroups);
  for (const auto &prop : world.props) {
    if (prop.skybox)
      continue; // submitted separately by the engine's sky camera
    if (!prop.model) {
      required.push_back(UINT32_MAX); // fail closed on an invalid retained prop
      continue;
    }
    for (const auto &part : prop.model->parts)
      if (!part.indices.empty())
        required.push_back(uint32_t(part.material));
  }
  std::sort(required.begin(), required.end());
  required.erase(std::unique(required.begin(), required.end()), required.end());
  return required;
}

template <class Materials>
std::optional<uint32_t> firstUnreadyBakeMaterial(const std::vector<uint32_t> &required,
                                               const Materials &materials) {
  for (uint32_t index : required)
    if (index >= materials.size() || !materials[index].resolved)
      return index;
  return {};
}

} // namespace proxy::bsp
