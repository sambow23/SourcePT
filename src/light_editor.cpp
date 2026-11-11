#include "light_editor.h"

#include "bsp_world.h"
#include "game.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace proxy::light_editor {
namespace {

using lighting::LightParams;
using lighting::SourceLight;

// ---- the runtime's ImGui exports (api/imgui_compat.h) ----
struct ImGuiApi {
  using Callback = void (*)(void *);
  void (*registerDraw)(Callback, void *) = nullptr;
  void (*registerOverlay)(Callback, void *) = nullptr;
  void (*text)(const char *) = nullptr;
  void (*textColored)(float, float, float, float, const char *) = nullptr;
  void (*textWrapped)(const char *) = nullptr;
  int (*checkbox)(const char *, int *) = nullptr;
  int (*inputText)(const char *, char *, uint32_t, int) = nullptr;
  int (*beginChild)(const char *, float, float, int, int) = nullptr;
  void (*endChild)() = nullptr;
  int (*selectable)(const char *, int, int, float, float) = nullptr;
  int (*button)(const char *, float, float) = nullptr;
  void (*sameLine)(float, float) = nullptr;
  void (*separator)() = nullptr;
  void (*spacing)() = nullptr;
  int (*colorEdit3)(const char *, float *) = nullptr;
  int (*dragFloat)(const char *, float *, float, float, float, const char *) = nullptr;
  int (*sliderFloat)(const char *, float *, float, float, const char *) = nullptr;
  int (*radioButton)(const char *, int) = nullptr;
  void (*pushId)(int) = nullptr;
  void (*popId)() = nullptr;
  void (*setTooltip)(const char *) = nullptr;
  int (*isItemHovered)(int) = nullptr;
  void (*getDisplaySize)(float *, float *) = nullptr;
  void (*addCircle)(float, float, float, uint32_t, int, float) = nullptr;
  void (*addText)(float, float, uint32_t, const char *) = nullptr;
  void (*getMousePos)(float *, float *) = nullptr;
  int (*isMouseClicked)(int) = nullptr;
  int (*isAnyWindowHovered)() = nullptr;
  void (*setScrollHereY)(float) = nullptr;

  bool load(HMODULE runtime) {
    bool ok = true;
    auto get = [&](auto &fn, const char *name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
          reinterpret_cast<void *>(GetProcAddress(runtime, name)));
      ok &= fn != nullptr;
    };
    get(registerDraw, "remixapi_imgui_RegisterDrawCallback");
    get(registerOverlay, "remixapi_imgui_RegisterOverlayCallback");
    get(text, "remixapi_imgui_Text");
    get(textColored, "remixapi_imgui_TextColored");
    get(textWrapped, "remixapi_imgui_TextWrapped");
    get(checkbox, "remixapi_imgui_Checkbox");
    get(inputText, "remixapi_imgui_InputText");
    get(beginChild, "remixapi_imgui_BeginChild");
    get(endChild, "remixapi_imgui_EndChild");
    get(selectable, "remixapi_imgui_Selectable");
    get(button, "remixapi_imgui_Button");
    get(sameLine, "remixapi_imgui_SameLine");
    get(separator, "remixapi_imgui_Separator");
    get(spacing, "remixapi_imgui_Spacing");
    get(colorEdit3, "remixapi_imgui_ColorEdit3");
    get(dragFloat, "remixapi_imgui_DragFloat");
    get(sliderFloat, "remixapi_imgui_SliderFloat");
    get(radioButton, "remixapi_imgui_RadioButton");
    get(pushId, "remixapi_imgui_PushID_Int");
    get(popId, "remixapi_imgui_PopID");
    get(setTooltip, "remixapi_imgui_SetTooltip");
    get(isItemHovered, "remixapi_imgui_IsItemHovered");
    get(getDisplaySize, "remixapi_imgui_GetDisplaySize");
    get(addCircle, "remixapi_imgui_BackgroundDrawList_AddCircle");
    get(addText, "remixapi_imgui_BackgroundDrawList_AddText");
    get(getMousePos, "remixapi_imgui_GetMousePos");
    get(isMouseClicked, "remixapi_imgui_IsMouseClicked");
    get(isAnyWindowHovered, "remixapi_imgui_IsAnyWindowHovered");
    get(setScrollHereY, "remixapi_imgui_SetScrollHereY");
    return ok;
  }
} g_ui;

// ---- state (under g_lock: the UI callbacks may run on the runtime's thread) ----
struct Entry {
  uint64_t hash = 0;
  bsp::Light light;
  SourceLight source{};
  LightParams defaults;
  bool usable = false; // describeLight made something of it
  std::string name;
};

std::mutex g_lock;
std::string g_map;
std::vector<Entry> g_entries;
size_t g_lightCount = ~size_t(0);
float g_lightScale = 1, g_sunScale = 1;
// Edits by light hash (lights of this map not in the BSP any more are kept,
// so that saving does not lose them), their revisions, and what was saved.
std::unordered_map<uint64_t, LightParams> g_edits, g_saved;
std::unordered_map<uint64_t, uint64_t> g_revisions;
uint64_t g_nextRevision = 1;
std::string g_status = "";
// OPENREMIX_PROXY_LIGHT_EDIT_MODE=1: starts in edit mode.
bool g_editMode = [] {
  char v[8] = {};
  return GetEnvironmentVariableA("OPENREMIX_PROXY_LIGHT_EDIT_MODE", v, sizeof(v)) && v[0] == '1';
}();
uint64_t g_selected = 0;
bool g_scrollToSelected = false;
char g_filter[64] = {};
float g_view[16] = {}, g_projection[16] = {};
bool g_haveCamera = false;
std::deque<std::string> g_commands;
bool g_teleport = false;
const char *g_uiState = "not registered";

SourceLight toSource(const bsp::Light &b) {
  SourceLight l = {};
  l.type = b.type;
  std::memcpy(l.color, b.color, 12);
  std::memcpy(l.position, b.position, 12);
  std::memcpy(l.direction, b.direction, 12);
  l.attenuation0 = b.attenuation[0];
  l.attenuation1 = b.attenuation[1];
  l.attenuation2 = b.attenuation[2];
  l.thetaDot = b.thetaDot;
  l.phiDot = b.phiDot;
  l.forwardCutoff = b.forwardCutoff;
  return l;
}

const char *typeName(int type) {
  switch (type) {
  case 1: return "Point";
  case 2: return "Directional";
  case 3: return "Spot";
  case lighting::kSurface: return "Surface";
  default: return "Unknown";
  }
}

bool sameParams(const LightParams &a, const LightParams &b) {
  return a.type == b.type && a.enabled == b.enabled && std::memcmp(a.radiance, b.radiance, sizeof(a.radiance)) == 0 &&
         a.radius == b.radius && a.coneAngle == b.coneAngle && a.coneSoftness == b.coneSoftness &&
         a.focusExponent == b.focusExponent && a.volumetric == b.volumetric;
}

// ---- files ----
std::filesystem::path lightsDirectory() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path root = std::filesystem::path(exe).parent_path();
  std::error_code ec;
  if (std::filesystem::is_directory(root / L"garrysmod", ec))
    return root / L"garrysmod" / L"data" / L"openremix" / L"lights";
  return root / L"openremix" / L"lights";
}

std::filesystem::path mapFile(const std::string &map) {
  std::string safe = map;
  for (char &c : safe)
    if (!(std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.'))
      c = '_';
  return lightsDirectory() / (safe + ".txt");
}

// "openremix-map-lights 1", then per edited light: hash (hex) enabled type
// radiance r g b radius coneAngle coneSoftness focusExponent volumetric.
void loadEdits(const std::string &map) {
  g_edits.clear();
  g_saved.clear();
  if (map.empty())
    return;
  FILE *f = _wfopen(mapFile(map).c_str(), L"r");
  if (!f)
    return;
  char line[512];
  bool header = false;
  while (std::fgets(line, sizeof(line), f)) {
    if (!header) {
      header = std::strncmp(line, "openremix-map-lights 1", 22) == 0;
      if (!header)
        break;
      continue;
    }
    unsigned long long hash = 0;
    int enabled = 1;
    LightParams p;
    if (std::sscanf(line, "%llx %d %d %f %f %f %f %f %f %f %f", &hash, &enabled, &p.type, &p.radiance[0],
                    &p.radiance[1], &p.radiance[2], &p.radius, &p.coneAngle, &p.coneSoftness, &p.focusExponent,
                    &p.volumetric) != 11 ||
        p.type < 1 || p.type > lighting::kSurface)
      continue;
    p.enabled = enabled != 0;
    g_edits[hash] = p;
  }
  std::fclose(f);
  g_saved = g_edits;
  g_status = header ? "loaded " + std::to_string(g_edits.size()) + " edited lights" : "saved file not readable";
}

bool saveEdits() {
  if (g_map.empty())
    return false;
  const auto path = mapFile(g_map);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const auto temporary = std::filesystem::path(path).concat(L".tmp");
  FILE *f = _wfopen(temporary.c_str(), L"w");
  if (!f) {
    g_status = "could not write " + path.string();
    return false;
  }
  std::fprintf(f, "openremix-map-lights 1\n");
  for (const auto &[hash, p] : g_edits)
    std::fprintf(f, "%016llx %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", (unsigned long long)hash,
                 int(p.enabled), p.type, p.radiance[0], p.radiance[1], p.radiance[2], p.radius, p.coneAngle,
                 p.coneSoftness, p.focusExponent, p.volumetric);
  const bool written = std::fflush(f) == 0;
  std::fclose(f);
  if (!written || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    g_status = "could not write " + path.string();
    return false;
  }
  if (g_edits.empty()) // nothing to keep: no file
    std::filesystem::remove(path, ec);
  g_saved = g_edits;
  g_status = "saved " + std::to_string(g_edits.size()) + " edited lights to " + path.string();
  return true;
}

bool unsaved() {
  if (g_edits.size() != g_saved.size())
    return true;
  for (const auto &[hash, p] : g_edits) {
    const auto found = g_saved.find(hash);
    if (found == g_saved.end() || !sameParams(found->second, p))
      return true;
  }
  return false;
}

void setEdit(const Entry &e, const LightParams &p) {
  if (sameParams(p, e.defaults))
    g_edits.erase(e.hash);
  else
    g_edits[e.hash] = p;
  g_revisions[e.hash] = g_nextRevision++;
}

// ---- the camera ----
// Screen position of a world point: false behind the camera. w: clip w.
bool project(const float p[3], float width, float height, float &x, float &y, float &w) {
  float view[4] = {}, clip[4] = {};
  const float in[4] = {p[0], p[1], p[2], 1};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      view[c] += in[r] * g_view[r * 4 + c];
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      clip[c] += view[r] * g_projection[r * 4 + c];
  w = clip[3];
  if (!(w > 1e-3f))
    return false;
  x = (clip[0] / w * .5f + .5f) * width;
  y = (.5f - clip[1] / w * .5f) * height;
  return std::isfinite(x) && std::isfinite(y);
}

void eyePosition(float eye[3]) {
  for (int i = 0; i < 3; ++i)
    eye[i] = -(g_view[12] * g_view[i * 4] + g_view[13] * g_view[i * 4 + 1] + g_view[14] * g_view[i * 4 + 2]);
}

// ---- teleport ----
void teleportTo(const Entry &e) {
  if (e.light.type == 2)
    return;
  const float *target = e.light.position;
  float eye[3] = {target[0] - 128, target[1], target[2]};
  if (g_haveCamera) {
    float from[3];
    eyePosition(from);
    float d[3] = {from[0] - target[0], from[1] - target[1], 0};
    const float length = std::hypot(d[0], d[1]);
    const float distance = std::max(96.f, 6 * (g_edits.count(e.hash) ? g_edits[e.hash].radius : e.defaults.radius));
    if (length > 1) {
      eye[0] = target[0] + d[0] / length * distance;
      eye[1] = target[1] + d[1] / length * distance;
    }
  }
  const float to[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  constexpr float kDegrees = 57.2957795f;
  const float yaw = std::atan2(to[1], to[0]) * kDegrees;
  const float pitch = -std::atan2(to[2], std::hypot(to[0], to[1])) * kDegrees;
  char command[200];
  // setpos places the feet: the eye is 64 units above them standing.
  std::snprintf(command, sizeof(command), "setpos %.1f %.1f %.1f; setang %.1f %.1f 0\n", eye[0], eye[1], eye[2] - 64,
                pitch, yaw);
  g_commands.push_back(command);
  g_status = std::string("teleported to ") + e.name + " (setpos needs sv_cheats 1)";
}

// ---- the map ----
void follow() {
  const std::string &map = bsp::mapName();
  const auto &lights = bsp::lights();
  if (map == g_map && lights.size() == g_lightCount)
    return;
  if (map != g_map) {
    g_map = map;
    g_selected = 0;
    g_revisions.clear();
    loadEdits(map);
  }
  g_lightCount = lights.size();
  g_entries.clear();
  int numbers[5] = {};
  for (const auto &b : lights) {
    Entry e;
    e.hash = b.hash;
    e.light = b;
    e.source = toSource(b);
    e.usable = lighting::describeLight(e.source, e.defaults, g_lightScale, g_sunScale);
    char name[96];
    const int type = b.type >= 1 && b.type <= lighting::kSurface ? b.type : 0;
    if (b.style)
      std::snprintf(name, sizeof(name), "%s %d (style %d)", typeName(b.type), ++numbers[type], b.style);
    else
      std::snprintf(name, sizeof(name), "%s %d", typeName(b.type), ++numbers[type]);
    e.name = name;
    g_entries.push_back(std::move(e));
  }
  // The lights were made again: every edited one is created anew.
  for (auto &[hash, rev] : g_revisions)
    rev = g_nextRevision++;
}

Entry *find(uint64_t hash) {
  for (auto &e : g_entries)
    if (e.hash == hash)
      return &e;
  return nullptr;
}

LightParams current(const Entry &e) {
  const auto found = g_edits.find(e.hash);
  return found != g_edits.end() ? found->second : e.defaults;
}

// ---- the panel (Plugin tab) ----
void drawControls(Entry &e) {
  LightParams p = current(e);
  bool changed = false;
  char text[200];
  std::snprintf(text, sizeof(text), "%s  at %.0f %.0f %.0f%s", e.name.c_str(), e.light.position[0],
                e.light.position[1], e.light.position[2], g_edits.count(e.hash) ? "  (edited)" : "");
  g_ui.text(text);
  if (g_teleport && e.light.type != 2) {
    if (g_ui.button("Teleport", 0, 0))
      teleportTo(e);
    if (g_ui.isItemHovered(0))
      g_ui.setTooltip("Places the player in front of the light (setpos and setang: needs sv_cheats 1).");
    g_ui.sameLine(0, -1);
  }
  if (g_ui.button("Revert to map", 0, 0) && g_edits.count(e.hash)) {
    g_edits.erase(e.hash);
    g_revisions[e.hash] = g_nextRevision++;
    return;
  }
  int enabled = p.enabled;
  if (g_ui.checkbox("Enabled", &enabled)) {
    p.enabled = enabled != 0;
    changed = true;
  }
  g_ui.text("Type");
  const int types[] = {1, 3, 2, lighting::kSurface};
  for (int i = 0; i < 4; ++i) {
    if (i)
      g_ui.sameLine(0, -1);
    if (g_ui.radioButton(typeName(types[i]), p.type == types[i]) && p.type != types[i]) {
      const int previous = p.type;
      p.type = types[i];
      if (p.type == 2 && previous != 2)
        p.radius = .5f;
      else if (p.type != 2 && previous == 2)
        p.radius = 2;
      if ((p.type == 3 || p.type == lighting::kSurface) && !(p.coneAngle > 0)) {
        p.coneAngle = p.type == 3 ? 45 : 90;
        p.focusExponent = p.type == lighting::kSurface ? 1 : 0;
      }
      changed = true;
    }
  }
  if (g_ui.isItemHovered(0))
    g_ui.setTooltip("Surface: a light emitting from one side (VRAD's emit_surface), as a spot of 90 degrees.");
  // Radiance as colour x intensity (the brightest channel).
  float intensity = std::max({p.radiance[0], p.radiance[1], p.radiance[2]});
  float color[3] = {1, 1, 1};
  if (intensity > 0)
    for (int k = 0; k < 3; ++k)
      color[k] = p.radiance[k] / intensity;
  if (g_ui.colorEdit3("Color", color)) {
    const float scale = intensity > 0 ? intensity : 1;
    for (int k = 0; k < 3; ++k)
      p.radiance[k] = std::max(color[k], 0.f) * scale;
    changed = true;
  }
  if (g_ui.dragFloat("Intensity", &intensity, std::max(intensity * .01f, .01f), 0, 1e7f, "%.2f")) {
    for (int k = 0; k < 3; ++k)
      p.radiance[k] = color[k] * std::max(intensity, 0.f);
    changed = true;
  }
  if (g_ui.isItemHovered(0))
    g_ui.setTooltip("Radiance of the brightest channel (Remix units). Radiance stays as set when the radius changes.");
  if (p.type == 2) {
    changed |= g_ui.dragFloat("Angular diameter", &p.radius, .01f, .01f, 45, "%.2f deg") != 0;
  } else {
    changed |= g_ui.dragFloat("Radius", &p.radius, .05f, .01f, 512, "%.2f") != 0;
    if (g_ui.isItemHovered(0))
      g_ui.setTooltip("Emitter radius in Source units: larger is softer and, at the same radiance, brighter.");
  }
  if (p.type == 3 || p.type == lighting::kSurface) {
    changed |= g_ui.sliderFloat("Cone angle", &p.coneAngle, 1, 180, "%.1f deg") != 0;
    changed |= g_ui.sliderFloat("Cone softness", &p.coneSoftness, 0, 1, "%.2f") != 0;
    changed |= g_ui.sliderFloat("Focus exponent", &p.focusExponent, 0, 10, "%.2f") != 0;
  }
  changed |= g_ui.sliderFloat("Volumetric", &p.volumetric, 0, 10, "%.2f") != 0;
  if (g_ui.isItemHovered(0))
    g_ui.setTooltip("How much the light scatters in volumetric fog (1: as bright as on surfaces).");
  if (changed)
    setEdit(e, p);
}

void drawPanel(void *) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_map.empty() || g_entries.empty()) {
    g_ui.textWrapped("Map lights: no map with lights is loaded (the BSP world provides them).");
    return;
  }
  char text[300];
  std::snprintf(text, sizeof(text), "Map lights: %s, %zu lights, %zu edited%s", g_map.c_str(), g_entries.size(),
                g_edits.size(), unsaved() ? " (unsaved changes)" : "");
  g_ui.text(text);
  int editMode = g_editMode;
  if (g_ui.checkbox("Edit mode", &editMode))
    g_editMode = editMode != 0;
  if (g_ui.isItemHovered(0))
    g_ui.setTooltip("Outlines the map's lights in the view; click one to select it.");
  g_ui.sameLine(0, -1);
  if (g_ui.button("Save", 0, 0))
    saveEdits();
  if (g_ui.isItemHovered(0))
    g_ui.setTooltip(("Saves this map's edits to " + mapFile(g_map).string()).c_str());
  g_ui.sameLine(0, -1);
  if (g_ui.button("Revert all", 0, 0)) {
    for (const auto &[hash, p] : g_edits)
      g_revisions[hash] = g_nextRevision++;
    g_edits.clear();
  }
  if (g_ui.isItemHovered(0))
    g_ui.setTooltip("Every light as the map made it (Save to keep that).");
  g_ui.sameLine(0, -1);
  if (g_ui.button("Reload saved", 0, 0)) {
    for (const auto &[hash, p] : g_edits)
      g_revisions[hash] = g_nextRevision++;
    loadEdits(g_map);
    for (const auto &[hash, p] : g_edits)
      g_revisions[hash] = g_nextRevision++;
  }
  if (!g_status.empty())
    g_ui.textColored(.6f, .6f, .6f, 1, g_status.c_str());
  g_ui.inputText("Filter", g_filter, sizeof(g_filter), 0);
  if (g_ui.beginChild("Map light list", 0, 220, 1, 0)) {
    std::string filter = g_filter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    int row = 0;
    for (auto &e : g_entries) {
      std::string name = e.name;
      std::string lowered = name;
      std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                     [](char c) { return char(std::tolower(uint8_t(c))); });
      if (!filter.empty() && lowered.find(filter) == std::string::npos)
        continue;
      const auto edit = g_edits.find(e.hash);
      if (edit != g_edits.end())
        name += edit->second.enabled ? "  *" : "  (off) *";
      if (!e.usable)
        name += "  (not submitted)";
      g_ui.pushId(row++);
      if (g_ui.selectable(name.c_str(), e.hash == g_selected, 0, 0, 0))
        g_selected = e.hash;
      if (e.hash == g_selected && g_scrollToSelected) {
        g_ui.setScrollHereY(.5f);
        g_scrollToSelected = false;
      }
      g_ui.popId();
    }
  }
  g_ui.endChild();
  g_ui.separator();
  if (Entry *e = find(g_selected); e && e->usable)
    drawControls(*e);
  else
    g_ui.textWrapped("Select a light in the list, or in the view with Edit mode on.");
}

// ---- the viewport (overlay callback) ----
void drawOverlay(void *) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!g_editMode || !g_haveCamera || g_entries.empty())
    return;
  float width = 0, height = 0;
  g_ui.getDisplaySize(&width, &height);
  if (!(width > 0 && height > 0))
    return;
  float mouse[2];
  g_ui.getMousePos(&mouse[0], &mouse[1]);
  const bool pick = g_ui.isMouseClicked(0) && !g_ui.isAnyWindowHovered();
  uint64_t picked = 0;
  float best = 1e30f;
  const float focal = g_projection[5] * height * .5f;
  for (const auto &e : g_entries) {
    if (!e.usable || e.light.type == 2)
      continue;
    float x, y, w;
    if (!project(e.light.position, width, height, x, y, w))
      continue;
    const LightParams p = current(e);
    const float radius = std::clamp(p.radius * focal / w, 6.f, 160.f);
    if (x < -radius || y < -radius || x > width + radius || y > height + radius)
      continue;
    const bool selected = e.hash == g_selected;
    const uint32_t alpha = selected ? 255 : 170;
    // ImGui colours are ABGR.
    uint32_t color = !p.enabled ? 0x808080u : g_edits.count(e.hash) ? 0x40c0ffu : 0xffffffu;
    if (selected)
      color = 0x00e0ffu;
    g_ui.addCircle(x, y, radius, color | alpha << 24, 0, selected ? 3.f : 1.5f);
    if (selected)
      g_ui.addText(x + radius + 4, y - 7, 0xff00e0ffu, e.name.c_str());
    if (pick) {
      const float d = std::hypot(mouse[0] - x, mouse[1] - y);
      // The nearest outline the click is inside (or near a small one); the
      // closer light of two at the same place.
      if (d <= std::max(radius, 12.f)) {
        const float score = d / std::max(radius, 12.f) + w * 1e-6f;
        if (score < best) {
          best = score;
          picked = e.hash;
        }
      }
    }
  }
  if (picked) {
    g_selected = picked;
    g_scrollToSelected = true;
  }
}

} // namespace

void sync(float lightScale, float sunScale) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_lightScale = lightScale;
  g_sunScale = sunScale;
  follow();
  static bool attempted = false;
  if (!attempted) {
    attempted = true;
    const HMODULE runtime = game::runtimeModule();
    if (!runtime || !g_ui.load(runtime)) {
      g_uiState = "the runtime lacks the ImGui plugin exports";
      return;
    }
    g_ui.registerDraw(&drawPanel, nullptr);
    g_ui.registerOverlay(&drawOverlay, nullptr);
    g_uiState = "registered";
  }
}

bool edited(uint64_t hash, LightParams &out, uint64_t &revision) {
  std::lock_guard<std::mutex> lock(g_lock);
  const auto found = g_edits.find(hash);
  if (found == g_edits.end())
    return false;
  out = found->second;
  const auto rev = g_revisions.find(hash);
  revision = rev == g_revisions.end() ? 0 : rev->second;
  return true;
}

void setCamera(const float view[16], const float projection[16]) {
  std::lock_guard<std::mutex> lock(g_lock);
  std::memcpy(g_view, view, sizeof(g_view));
  std::memcpy(g_projection, projection, sizeof(g_projection));
  g_haveCamera = true;
}

bool takeCommand(std::string &command) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_commands.empty())
    return false;
  command = std::move(g_commands.front());
  g_commands.pop_front();
  return true;
}

void dumpStatistics(FILE *out) {
  std::lock_guard<std::mutex> lock(g_lock);
  size_t disabled = 0;
  for (const auto &[hash, p] : g_edits)
    disabled += !p.enabled;
  fprintf(out,
          "== Map light editor (developer panel, Plugin tab): %s | map %s: %zu lights, %zu edited (%zu disabled)%s | "
          "edit mode %d, selected %016llx, teleport %s | file %s | %s\n",
          g_uiState, g_map.empty() ? "(none)" : g_map.c_str(), g_entries.size(), g_edits.size(), disabled,
          unsaved() ? ", unsaved" : "", int(g_editMode), (unsigned long long)g_selected,
          g_teleport ? "available" : "unavailable", g_map.empty() ? "-" : mapFile(g_map).string().c_str(),
          g_status.c_str());
  // OPENREMIX_PROXY_LIGHT_LIST=1: every light (hash, name, place, default
  // parameters), for writing a map's file by hand.
  static const bool list = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_LIGHT_LIST", v, sizeof(v)) && v[0] == '1';
  }();
  if (list)
    for (const auto &e : g_entries)
      fprintf(out, "   light %016llx %s at %.0f %.0f %.0f: %s radiance %.3g %.3g %.3g radius %.3g cone %.3g%s\n",
              (unsigned long long)e.hash, e.name.c_str(), e.light.position[0], e.light.position[1],
              e.light.position[2], typeName(e.defaults.type), e.defaults.radiance[0], e.defaults.radiance[1],
              e.defaults.radiance[2], e.defaults.radius, e.defaults.coneAngle, e.usable ? "" : " (not submitted)");
}

void setTeleportAvailable(bool available) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_teleport = available;
}

} // namespace proxy::light_editor
