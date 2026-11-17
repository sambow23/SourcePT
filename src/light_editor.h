#pragma once
// The map light editor: the BSP world's lights (bsp::lights) adjusted in the
// developer panel's Plugin tab (the runtime's remixapi_imgui_* exports).
//
//  * Edit mode outlines every light in the view (the overlay callback, over
//    the image and under the panel) and selects the one clicked.
//  * A list of the map's lights, filtered by name; Teleport places the player
//    in front of the selected light (setpos/setang from the main thread).
//  * Per light: enabled, radiance (colour and intensity), emitter radius
//    (Remix terms: a sphere's radius, a directional light's angular
//    diameter), type (point, spot, directional, surface), the cone of a spot
//    and the volumetric radiance scale. Edits apply at once.
//  * Saved per map to garrysmod/data/openremix/lights/<map>.txt (the game
//    root's openremix/lights in other games), and loaded with the map.
//
// The tab (named "Garry's Mod" where the runtime lets a plugin name it, and
// "Plugin" in older ones) is in sections, the runtime's panels where it has
// them for a plugin: Lighting holds the wrapper's options (particleLights;
// saved as they change, to openremix/options.txt beside the lights
// directory), Map lights the editor.
//
// Everything but takeCommand and particleLights runs on the thread that presents (the light
// submission and the runtime's UI callbacks).
#include "source_lights.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace proxy::light_editor {

// Before the map's lights are submitted each frame: follows the loaded map
// (its saved edits), registers the panel callbacks once the runtime is up.
// lightScale/sunScale: the scales the lights are created with.
void sync(float lightScale, float sunScale);

// The edited parameters of a map light (by bsp::Light::hash): false when it
// is as the map made it. revision changes with every edit of it.
bool edited(uint64_t hash, lighting::LightParams &out, uint64_t &revision);

// The world camera this frame (D3D row-vector view and projection), for the
// outlines and picking.
void setCamera(const float view[16], const float projection[16]);

// Main thread: the next console command the editor queued (teleport); false
// when none. teleportAvailable: whether anything runs them.
bool takeCommand(std::string &command);
void setTeleportAvailable(bool available);

// Any thread: whether glowing particles light the scene (remix_output's
// particle lights). The tab's option, on unless it was saved off;
// OPENREMIX_PROXY_PARTICLE_LIGHTS=0 turns them off whatever it says.
bool particleLights();

// Statistics thread: the editor's state (shaderapi_proxy_slots.txt).
void dumpStatistics(FILE *out);

} // namespace proxy::light_editor
