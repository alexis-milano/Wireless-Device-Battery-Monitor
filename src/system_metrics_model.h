#pragma once
// Platform-free parts of the system monitor (CPU / memory / GPU load and temperatures), kept in a
// header so tests/system_metrics_test.cpp can check them without Windows APIs.
#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace system_metrics {

// One sample of every metric. A negative value means "not available" (no sensor, no source running).
struct Snapshot {
    int cpuPercent = -1;
    int memPercent = -1;
    int gpuPercent = -1;
    int cpuTempC = -1;
    int gpuTempC = -1;
};

// CPU load from two GetSystemTimes() readings: kernel time includes idle time.
inline int CpuPercent(uint64_t idle0, uint64_t kernel0, uint64_t user0,
                      uint64_t idle1, uint64_t kernel1, uint64_t user1) {
    if (idle1 < idle0 || kernel1 < kernel0 || user1 < user0) return -1;  // counter reset
    const uint64_t idle = idle1 - idle0;
    const uint64_t total = (kernel1 - kernel0) + (user1 - user0);
    if (total == 0 || idle > total) return -1;
    return static_cast<int>((100 * (total - idle) + total / 2) / total);
}

// GPU load like Task Manager: per adapter, the busiest engine type (3D, Copy, VideoDecode, ...), where an
// engine type's load is the sum over the processes using it. Instance names of the
// "\GPU Engine(*)\Utilization Percentage" counter look like
// "pid_1234_luid_0x00000000_0x0000D1E3_phys_0_eng_0_engtype_3D".
struct EngineSample {
    std::wstring instance;
    double percent = 0;
};

inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// "luid_0x..._0x..._phys_N" plus "engtype_X": the key that groups a process's share of one engine type.
inline bool EngineKey(const std::wstring& instance, std::wstring& adapter, std::wstring& engine) {
    const std::wstring s = Lower(instance);
    const size_t luid = s.find(L"luid_");
    const size_t eng = s.find(L"_eng_");
    const size_t type = s.find(L"engtype_");
    if (luid == std::wstring::npos || eng == std::wstring::npos || type == std::wstring::npos || eng < luid) return false;
    adapter = s.substr(luid, eng - luid);
    engine = s.substr(type + 8);
    // The engine index matters: two "3D" engines of one adapter are separate engines.
    const size_t engEnd = s.find(L'_', eng + 5);
    engine += L"#" + s.substr(eng + 5, engEnd == std::wstring::npos ? std::wstring::npos : engEnd - eng - 5);
    return !engine.empty();
}

inline int GpuPercent(const std::vector<EngineSample>& samples) {
    std::map<std::wstring, std::map<std::wstring, double>> load;  // adapter -> engine -> summed load
    bool any = false;
    for (const auto& s : samples) {
        std::wstring adapter, engine;
        if (!EngineKey(s.instance, adapter, engine) || s.percent < 0) continue;
        load[adapter][engine] += s.percent;
        any = true;
    }
    if (!any) return -1;
    double best = 0;
    for (const auto& [adapter, engines] : load)
        for (const auto& [engine, pct] : engines) best = std::max(best, pct);
    return static_cast<int>(std::min(100.0, best) + 0.5);
}

// Picks the CPU temperature among the sensor labels LibreHardwareMonitor or HWiNFO publish. Lower rank wins:
// the package / Tctl reading is what tools and BIOSes call "the CPU temperature"; a core maximum is next.
inline int CpuTempLabelRank(const std::wstring& label) {
    const std::wstring s = Lower(label);
    if (s.find(L"package") != std::wstring::npos) return 0;         // Intel "CPU Package"
    if (s.find(L"tctl") != std::wstring::npos || s.find(L"tdie") != std::wstring::npos) return 1;  // AMD
    if (s.find(L"core max") != std::wstring::npos) return 2;
    if (s.find(L"cpu") != std::wstring::npos && s.find(L"gpu") == std::wstring::npos &&
        s.find(L"distance") == std::wstring::npos && s.find(L"vr") == std::wstring::npos) return 3;  // "CPU", "CPU (Tdie)"
    if (s.find(L"core") != std::wstring::npos && s.find(L"distance") == std::wstring::npos) return 4;  // "Core #0"
    return -1;
}

struct TempReading {
    std::wstring label;
    double celsius = 0;
};

inline int PickCpuTemp(const std::vector<TempReading>& readings) {
    int bestRank = 1000;
    double best = -1;
    for (const auto& r : readings) {
        const int rank = CpuTempLabelRank(r.label);
        if (rank < 0 || r.celsius <= 0 || r.celsius > 150) continue;
        if (rank < bestRank || (rank == bestRank && r.celsius > best)) { bestRank = rank; best = r.celsius; }
    }
    return best < 0 ? -1 : static_cast<int>(best + 0.5);
}

// D3DKMT reports the GPU temperature in tenths of a degree; 0 means the driver does not expose it.
inline int GpuTempFromDeciCelsius(unsigned long deci) {
    if (deci == 0 || deci > 1500) return -1;
    return static_cast<int>((deci + 5) / 10);
}

}  // namespace system_metrics
