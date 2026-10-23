#pragma once

// The launcher's operating-system pieces (kept out of launcher.cpp: <windows.h> and raylib clash).
#include <filesystem>

namespace godhand::launcher::platform
{
    // A file (folder = false: disc images) or folder picker over the window. Empty if cancelled or not available.
    std::filesystem::path pick(void *window, bool folder);
    // Opens a file or folder with the system's handler (the controls file in a text editor).
    void open(const std::filesystem::path &path);
    // Shift held right now (the launcher shows even when hidden).
    bool shiftHeld();
}
