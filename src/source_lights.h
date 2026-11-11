#pragma once
#include <cstdint>
#include <cstddef>
#include <remix/remix_c.h>

namespace proxy::lighting {
// Types 1..3 and this 76-byte prefix match Source's 88-byte LightDesc_t.
// The bridge skips its unused trailing fields when advancing engine arrays.
// Surface is an internal BSP tag; it never comes from the engine ABI.
constexpr int kSurface = 4;
struct SourceLight {
  int32_t type;
  float color[3], position[3], direction[3];
  float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi, thetaDot, phiDot;
  // Projected cookie metadata follows the engine ABI prefix.
  float cookieUp[3] = {}, cookieFov = 0, forwardCutoff = 0;
  uint64_t cookie = 0;
};
static_assert(offsetof(SourceLight, cookieUp) == 76 && offsetof(SourceLight, cookie) == 96 &&
              sizeof(SourceLight) == 104);

// The Remix light a Source light becomes, in the terms a person edits
// (light_editor): its type (SourceLight's: 1 point, 2 directional, 3 spot,
// kSurface), radiance, emitter radius (a sphere's, in Source units; a
// directional light's angular diameter in degrees), shaping (spot and
// surface lights) and volumetric radiance scale. A disabled light keeps its
// slot with no radiance. Position, direction, cookie and forward cutoff stay
// the Source light's.
struct LightParams {
  int32_t type = 1;
  float radiance[3] = {};
  float radius = 2;
  float coneAngle = 0, coneSoftness = 0, focusExponent = 0;
  float volumetric = 1;
  bool enabled = true;
};

// The parameters createLight gives a Source light; false when it makes none
// (an unknown type, no direction where one is needed, no finite radiance).
bool describeLight(const SourceLight &light, LightParams &out, float lightScale = 1, float sunScale = 1);

// Shared by the engine bridge and the standalone map host. Positions, emitter
// radius and attenuation distances all remain in Source units.
remixapi_LightHandle createLight(remixapi_Interface &api, const SourceLight &light, uint64_t hash,
                                float lightScale = 1, float sunScale = 1);
// The light with these parameters in place of describeLight's (an edited light).
remixapi_LightHandle createLight(remixapi_Interface &api, const SourceLight &light, const LightParams &params,
                                uint64_t hash);
} // namespace proxy::lighting
