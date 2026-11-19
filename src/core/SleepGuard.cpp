#include "core/SleepGuard.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool SleepGuard::setActive(bool on)
{
    if (on == m_active)
        return true;
#ifdef _WIN32
    // ES_CONTINUOUS keeps the state until changed; without ES_SYSTEM_REQUIRED it clears it.
    const EXECUTION_STATE flags = on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS;
    if (SetThreadExecutionState(flags) == 0)
        return false;
#endif
    m_active = on;
    return true;
}
