#pragma once

// Keeps the computer awake while downloads run (the display may still turn off).
// Windows: SetThreadExecutionState(ES_SYSTEM_REQUIRED) on the calling thread, so always use
// it from one thread (the GUI thread). A no-op on other platforms for now.
class SleepGuard {
public:
    SleepGuard() = default;
    ~SleepGuard() { setActive(false); }
    SleepGuard(const SleepGuard&) = delete;
    SleepGuard& operator=(const SleepGuard&) = delete;

    // Returns false if the system refused the request.
    bool setActive(bool on);
    bool active() const { return m_active; }

private:
    bool m_active = false;
};
