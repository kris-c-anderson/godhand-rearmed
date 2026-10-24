#pragma once

namespace godhand
{
    // Crashes the in-process reporter (main.cpp crashLogFilter) cannot see (crash_history.cpp): a fail-fast (abort()
    // or std::terminate inside a DLL with its own C runtime, a security check), heap corruption, a stack overflow.
    // Windows' error reporting still records them; at start, a background thread reads its last records for
    // godhand.exe from the Application event log and writes them to the log (module, offset, whether it was this
    // build). Also logs heap corruption and stack overflow as they are raised (a vectored handler), before
    // Windows ends the process. Windows only; no-ops elsewhere.
    void reportPreviousCrashes();
    void installFatalExceptionLog();
    // A watcher thread: when the game window stops taking messages (Windows' "Not responding"), every thread's stack
    // goes to the log once.
    void startWindowWatch();
}
