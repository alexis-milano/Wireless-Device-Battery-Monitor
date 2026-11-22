#pragma once
// Optional system monitor for the taskbar widget: CPU, memory and GPU load, CPU and GPU temperature.
// Sampled on its own thread every two seconds, only while at least one metric is switched on.
//  * CPU / memory: GetSystemTimes / GlobalMemoryStatusEx.
//  * GPU load: the "GPU Engine" performance counters Task Manager reads (any vendor).
//  * GPU temperature: D3DKMT adapter performance data (WDDM 2.4+ drivers, as in Task Manager).
//  * CPU temperature: Windows has no user-mode API for it, so it is read from LibreHardwareMonitor /
//    OpenHardwareMonitor (WMI) or HWiNFO (shared memory) when one of them is running. No driver is installed.
#include <windows.h>
#include "system_metrics_model.h"

namespace system_metrics {

struct Wanted {
    bool cpu = false, mem = false, gpu = false, cpuTemp = false, gpuTemp = false;
    bool Any() const { return cpu || mem || gpu || cpuTemp || gpuTemp; }
};

// Starts the sampling thread; `msg` is posted to `notify` whenever a new snapshot differs from the last one.
void Start(HWND notify, UINT msg);
void SetWanted(const Wanted& wanted);
Snapshot Latest();
void Stop();

}  // namespace system_metrics
