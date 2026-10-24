#pragma once

namespace godhand
{
    // Crashes the in-process reporter (main.cpp crashLogFilter) cannot see (crash_history.cpp): a fail-fast (abort()
    // or std::terminate inside a DLL with its own C runtime, a security check), heap corruption, a stack overflow.
    // Windows' error reporting still records them; at start, a background thread reads its last records for
    // godhand.exe from the Application event log and writes them to the log, the faulting address named when it is
    // in this build. Also logs heap corruption and stack overflow as they are raised (a vectored handler), before
    // Windows ends the process. Windows only; no-ops elsewhere.
    void reportPreviousCrashes();
    void installFatalExceptionLog();
}
