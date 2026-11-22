// GCC 16 flags initializer lists inside assert() as dangling; they live until the end of the full expression.
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#include "../src/system_metrics_model.h"
#include <cassert>
#include <cstdio>
using namespace system_metrics;

int main() {
    // CPU: 1000 ticks elapsed (kernel includes idle), 250 of them idle -> 75 %.
    assert(CpuPercent(100, 400, 100, 350, 1200, 300) == 75);
    assert(CpuPercent(100, 400, 100, 100, 400, 100) == -1);  // no time elapsed
    assert(CpuPercent(500, 400, 100, 100, 1200, 300) == -1);  // counter went backwards

    // GPU: per adapter the busiest engine; the processes sharing one engine add up.
    const std::wstring a = L"luid_0x00000000_0x0000D1E3_phys_0";
    assert(GpuPercent({
        { L"pid_10_" + a + L"_eng_0_engtype_3D", 20 },
        { L"pid_11_" + a + L"_eng_0_engtype_3D", 15 },
        { L"pid_11_" + a + L"_eng_3_engtype_VideoDecode", 30 },
        { L"pid_12_" + a + L"_eng_1_engtype_Copy", 5 },
    }) == 35);
    // Two engines of the same type are not summed together.
    assert(GpuPercent({
        { L"pid_1_" + a + L"_eng_0_engtype_3D", 40 },
        { L"pid_1_" + a + L"_eng_1_engtype_3D", 40 },
    }) == 40);
    assert(GpuPercent({ { L"pid_1_" + a + L"_eng_0_engtype_3D", 70 }, { L"pid_2_" + a + L"_eng_0_engtype_3D", 60 } }) == 100);
    assert(GpuPercent({}) == -1);
    assert(GpuPercent({ { L"_Total", 12 } }) == -1);

    // CPU temperature: package / Tctl first, then the hottest core, never a GPU or drive sensor.
    assert(PickCpuTemp({ { L"Core #0", 61 }, { L"CPU Package", 58.4 }, { L"GPU Core", 70 } }) == 58);
    assert(PickCpuTemp({ { L"Core #0", 61 }, { L"Core #1", 66.6 } }) == 67);
    assert(PickCpuTemp({ { L"CPU (Tctl/Tdie)", 72 }, { L"CPU CCD1 (Tdie)", 68 } }) == 72);
    assert(PickCpuTemp({ { L"GPU Temperature", 60 }, { L"Drive Temperature", 40 } }) == -1);
    assert(PickCpuTemp({ { L"CPU Package", 0 } }) == -1);

    assert(GpuTempFromDeciCelsius(553) == 55);
    assert(GpuTempFromDeciCelsius(0) == -1);

    std::puts("System monitor tests passed");
    return 0;
}
