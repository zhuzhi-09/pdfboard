#pragma once

#include <QString>
#include <windows.h>
#include <psapi.h>

struct MemInfo {
    double workingSetMB = 0.0;
    double privateMB    = 0.0;
};

// Private working set is the number Task Manager shows in its "Memory" column.
inline MemInfo currentMemInfo()
{
    MemInfo m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        m.workingSetMB = double(pmc.WorkingSetSize) / 1048576.0;
        m.privateMB    = double(pmc.PrivateUsage) / 1048576.0;
    }
    return m;
}
