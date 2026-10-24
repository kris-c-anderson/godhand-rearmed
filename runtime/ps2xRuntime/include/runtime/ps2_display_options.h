#pragma once

#include <cstdint>
#include <vector>

namespace ps2_host_display
{
    void setAspect(float aspect);
    float aspect();
    void setFitWindow(bool enabled);
    void setStartMaximized(bool enabled);
    // Borderless fullscreen at start (like F11).
    void setStartFullscreen(bool enabled);
    // Makes the game window (raylib) if it does not exist yet, sized by PS2X_WINDOW_SIZE, the aspect and the start
    // flags above; called again (by PS2Runtime::initialize after a launcher made the window), it keeps the window and
    // applies what changed. Main thread.
    void openHostWindow(const char *title);
    // Real vsync for the window (PS2X_VSYNC, default on); off = the software 60 FPS limiter.
    bool vsyncEnabled();
    // Game frames for the FPS overlay (F3): the port calls this once per iteration of the game's main loop.
    void noteGameFrame();
    // A line shown on screen (top left) until replaced or cleared with an empty string, e.g. "capturing profile".
    void setOverlayNote(const char *text);
    // The FPS overlay's latest once-per-second line ("Jogo N FPS | Tela N | Velocidade N% | pior quadro N ms"), and
    // a counter that changes each time it is refreshed.
    const char *fpsLine(unsigned &serial);

    // One record per real present (after the swap): host time (steady_clock, seconds since its epoch), the emulated
    // field (vsync tick), the GS image sequence shown (a new image or the previous one again) and the game-frame count.
    struct PresentRecord
    {
        double hostSeconds;
        uint64_t field;
        uint64_t imageSequence;
        uint64_t gameFrames;
    };
    // Copies the records made at or after `sinceHostSeconds` (a ring of the last 65536) into `out`.
    void copyPresents(double sinceHostSeconds, std::vector<PresentRecord> &out);

    // The host loop's heartbeat (the main thread: window messages, input, present): iterations so far and the
    // steady-clock milliseconds of its last window-message pump (0 before the first). A hang watch compares them
    // with what Windows says about the window.
    void hostLoopHeartbeat(uint64_t &iterations, uint64_t &lastPumpMs);
    uint64_t hostSteadyMs();
}
