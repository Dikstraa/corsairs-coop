// Names under other players' ships on the world map (drawn over the game's Direct3D 9 frame).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace coopnet
{
struct MapLabel
{
    float x, y, z;    // the ship's position in the map scene
    std::string text; // in the game's code page (1251)
};

// Redirects the device's EndScene once; false when that is not possible (labels are then not drawn).
bool HookMapLabels();

// The labels to draw from now on, and where this player's own ship is (the camera looks at it); a list
// not renewed for a moment (the map closed) is not drawn.
void SetMapLabels(const std::vector<MapLabel> &labels, float meX, float meZ);

// The player list shown while F3 is toggled on: lines split by '|', in the game's code page.
void SetPanel(const std::string &text);

// A line for coopnet.log (coopnet.cpp).
void LogLine(const char *line);
} // namespace coopnet
