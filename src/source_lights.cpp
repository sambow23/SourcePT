#include "source_lights.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <remix/remix_native_ext.h>

namespace proxy::lighting {
namespace {
constexpr float kPi = 3.14159265f;

bool unit(const float in[3], remixapi_Float3D &out) {
  const float length = std::hypot(in[0], in[1], in[2]);
  if (!std::isfinite(length) || !(length > 1e-6f))
    return false;
  out = {in[0] / length, in[1] / length, in[2] / length};
  return true;
}

bool cookieUsable(const SourceLight &light) { return light.cookie && light.cookieFov > 0 && light.cookieFov < 180; }
} // namespace

bool describeLight(const SourceLight &light, LightParams &out, float lightScale, float sunScale) {
  constexpr float radius = 2, reference = 100;
  if (light.type < 1 || light.type > kSurface)
    return false;
  remixapi_Float3D direction;
  const bool validDirection = unit(light.direction, direction);
  out = {};
  out.type = light.type;
  float scale;
  if (light.type == 2) {
    if (!validDirection)
      return false;
    scale = sunScale * kPi;
    out.radius = .5f; // angular diameter, degrees
  } else {
    const float falloff = light.attenuation0 + light.attenuation1 * reference +
                          light.attenuation2 * reference * reference;
    if (!(falloff > 0) || !std::isfinite(falloff))
      return false;
    // Preserve the existing radiant intensity at the reference distance.
    scale = lightScale * reference * reference / (falloff * kPi * radius * radius);
    out.radius = radius;
    if (light.type == kSurface) {
      if (!validDirection)
        return false;
      // VRAD's emit_surface uses max(dot(normal, outgoing), 0), not the
      // isotropic point approximation supplied to Source's model lighting.
      // See ValveSoftware/source-sdk-2013, src/utils/vrad/lightmap.cpp,
      // GatherSampleStandardLightSSE (emit_surface).
      // Existing spherical sampling retains soft shadows and one light ID;
      // shaping restores the authored angular distribution in every backend.
      out.coneAngle = 90;
      out.focusExponent = 1;
    } else if (light.type == 3 && validDirection && light.phiDot > -1 && light.phiDot < 1) {
      const float outer = std::acos(light.phiDot), inner = std::acos(std::clamp(light.thetaDot, -1.f, 1.f));
      out.coneAngle = outer * 180 / kPi;
      out.coneSoftness = outer > 0 ? std::clamp((outer - inner) / outer, 0.f, 1.f) : 0;
      if (cookieUsable(light)) {
        const float corner = std::atan(1.41421356f * std::tan(light.cookieFov * .5f * kPi / 180));
        out.coneAngle = std::min(corner * 180 / kPi + 1.f, 89.f);
        out.coneSoftness = 0;
      }
    } else if (light.type == 3) {
      out.type = 1; // no cone: a point light
    }
  }
  for (int k = 0; k < 3; ++k) {
    out.radiance[k] = light.color[k] * scale;
    if (!std::isfinite(out.radiance[k]) || out.radiance[k] < 0)
      return false;
  }
  return true;
}

remixapi_LightHandle createLight(remixapi_Interface &api, const SourceLight &light, const LightParams &params,
                                uint64_t hash) {
  if (params.type < 1 || params.type > kSurface)
    return nullptr;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = hash;
  remixapi_LightInfoSphereEXT sphere = {};
  remixapi_LightInfoForwardCutoffEXT cutoff{REMIXAPI_STRUCT_TYPE_LIGHT_INFO_FORWARD_CUTOFF_EXT, nullptr,
                                           light.forwardCutoff};
  remixapi_LightInfoBakingEXT baking{REMIXAPI_STRUCT_TYPE_LIGHT_BAKING_EXT, nullptr,
                                    REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT};
  remixapi_LightInfoCookieEXT cookie = {};
  wchar_t cookiePath[32] = {};
  remixapi_LightInfoDistantEXT distant = {};
  // A light edited into a type that needs a direction it never had looks down.
  remixapi_Float3D direction;
  if (!unit(light.direction, direction))
    direction = {0, 0, -1};
  if (params.type == 2) {
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = direction;
    distant.angularDiameterDegrees = std::clamp(params.radius, .01f, 45.f);
    distant.volumetricRadianceScale = std::max(params.volumetric, 0.f);
    info.pNext = &distant;
  } else {
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {light.position[0], light.position[1], light.position[2]};
    sphere.radius = std::max(params.radius, .01f);
    sphere.volumetricRadianceScale = std::max(params.volumetric, 0.f);
    if (params.type == kSurface)
      sphere.pNext = &baking;
    if (params.type == kSurface || params.type == 3) {
      sphere.shaping_hasvalue = 1;
      sphere.shaping_value.direction = direction;
      sphere.shaping_value.coneAngleDegrees = std::clamp(params.coneAngle, 0.f, 180.f);
      sphere.shaping_value.coneSoftness = std::clamp(params.coneSoftness, 0.f, 1.f);
      sphere.shaping_value.focusExponent = std::max(params.focusExponent, 0.f);
    }
    if (params.type == 3 && cookieUsable(light)) {
      swprintf(cookiePath, 32, L"0x%llX", static_cast<unsigned long long>(light.cookie));
      cookie.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT;
      cookie.texture = cookiePath;
      cookie.up = {light.cookieUp[0], light.cookieUp[1], light.cookieUp[2]};
      cookie.fovDegrees = light.cookieFov;
      cookie.pNext = sphere.pNext;
      sphere.pNext = &cookie;
    }
    if (light.forwardCutoff > 0 && (params.type == 1 || params.type == 3)) {
      cutoff.pNext = sphere.pNext;
      sphere.pNext = &cutoff;
    }
    info.pNext = &sphere;
  }
  const float on = params.enabled ? 1.f : 0.f;
  info.radiance = {params.radiance[0] * on, params.radiance[1] * on, params.radiance[2] * on};
  for (float value : {info.radiance.x, info.radiance.y, info.radiance.z})
    if (!std::isfinite(value) || value < 0)
      return nullptr;
  remixapi_LightHandle handle = nullptr;
  return api.CreateLight(&info, &handle) == REMIXAPI_ERROR_CODE_SUCCESS ? handle : nullptr;
}

remixapi_LightHandle createLight(remixapi_Interface &api, const SourceLight &light, uint64_t hash,
                                float lightScale, float sunScale) {
  LightParams params;
  if (!describeLight(light, params, lightScale, sunScale))
    return nullptr;
  return createLight(api, light, params, hash);
}
} // namespace proxy::lighting
