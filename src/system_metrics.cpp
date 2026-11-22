#include "system_metrics.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <wbemcli.h>
#include <oleauto.h>

#include <atomic>
#include <string>
#include <vector>

namespace system_metrics {
namespace {

// ---- CPU / memory ----------------------------------------------------------------------------------------

uint64_t U64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

struct CpuSampler {
    bool primed = false;
    uint64_t idle = 0, kernel = 0, user = 0;
    int Sample() {
        FILETIME fi, fk, fu;
        if (!GetSystemTimes(&fi, &fk, &fu)) return -1;
        const uint64_t i = U64(fi), k = U64(fk), u = U64(fu);
        const int pct = primed ? CpuPercent(idle, kernel, user, i, k, u) : -1;
        idle = i; kernel = k; user = u; primed = true;
        return pct;
    }
};

int MemoryPercent() {
    MEMORYSTATUSEX m{ sizeof(m) };
    return GlobalMemoryStatusEx(&m) ? static_cast<int>(m.dwMemoryLoad) : -1;
}

// ---- GPU load: performance counters ----------------------------------------------------------------------

struct GpuLoadSampler {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool primed = false;
    ULONGLONG retryAt = 0;
    ~GpuLoadSampler() { Close(); }
    void Close() {
        if (query) PdhCloseQuery(query);
        query = nullptr; counter = nullptr; primed = false;
    }
    bool Open() {
        if (query) return true;
        if (GetTickCount64() < retryAt) return false;
        retryAt = GetTickCount64() + 60000;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return false; }
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
            Close();
            return false;
        }
        primed = PdhCollectQueryData(query) == ERROR_SUCCESS;  // rates need two collections
        return true;
    }
    int Sample() {
        if (!Open()) return -1;
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return -1;
        if (!primed) { primed = true; return -1; }
        DWORD bytes = 0, count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
        if (st != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0) return count == 0 && st == ERROR_SUCCESS ? 0 : -1;
        std::vector<BYTE> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS)
            return -1;
        std::vector<EngineSample> samples;
        samples.reserve(count);
        for (DWORD i = 0; i < count; i++) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            samples.push_back({ items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue });
        }
        return GpuPercent(samples);
    }
};

// ---- GPU temperature: D3DKMT adapter performance data ----------------------------------------------------
// Declared here (not from d3dkmthk.h) and resolved from gdi32 at run time, so older Windows simply has no value.

typedef UINT KmtHandle;
typedef LONG KmtStatus;
struct KmtAdapterInfo {
    KmtHandle hAdapter;
    LUID AdapterLuid;
    ULONG NumOfSources;
    BOOL bPrecisePresentRegionsPreferred;
};
struct KmtEnumAdapters2 {
    ULONG NumAdapters;
    KmtAdapterInfo* pAdapters;
};
struct KmtQueryAdapterInfo {
    KmtHandle hAdapter;
    int Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};
struct KmtCloseAdapter {
    KmtHandle hAdapter;
};
struct KmtAdapterPerfData {
    UINT32 PhysicalAdapterIndex;
    ULONGLONG MemoryFrequency;
    ULONGLONG MaxMemoryFrequency;
    ULONGLONG MaxMemoryFrequencyOC;
    ULONGLONG MemoryBandwidth;
    ULONGLONG PCIEBandwidth;
    ULONG FanRPM;
    ULONG Power;
    ULONG Temperature;  // tenths of a degree Celsius
    UCHAR PowerStateOverride;
};
constexpr int kKmtQaiAdapterPerfData = 62;  // KMTQAITYPE_ADAPTERPERFDATA (WDDM 2.4)

struct GpuTempSampler {
    using EnumFn = KmtStatus(APIENTRY*)(KmtEnumAdapters2*);
    using QueryFn = KmtStatus(APIENTRY*)(const KmtQueryAdapterInfo*);
    using CloseFn = KmtStatus(APIENTRY*)(const KmtCloseAdapter*);
    EnumFn enumAdapters = nullptr;
    QueryFn query = nullptr;
    CloseFn close = nullptr;
    std::vector<KmtAdapterInfo> adapters;
    bool loaded = false;
    ULONGLONG enumeratedAt = 0;

    ~GpuTempSampler() { CloseAll(); }
    void CloseAll() {
        for (const auto& a : adapters) {
            KmtCloseAdapter c{ a.hAdapter };
            if (close) close(&c);
        }
        adapters.clear();
    }
    bool Load() {
        if (loaded) return enumAdapters && query && close;
        loaded = true;
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
        if (!gdi) return false;
        enumAdapters = reinterpret_cast<EnumFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTEnumAdapters2")));
        query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")));
        close = reinterpret_cast<CloseFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
        return enumAdapters && query && close;
    }
    void Enumerate() {
        CloseAll();
        enumeratedAt = GetTickCount64();
        KmtEnumAdapters2 e{ 0, nullptr };
        if (enumAdapters(&e) != 0 || e.NumAdapters == 0) return;
        adapters.resize(e.NumAdapters);
        e.pAdapters = adapters.data();
        if (enumAdapters(&e) != 0) { adapters.clear(); return; }
        adapters.resize(e.NumAdapters);
    }
    int Sample() {
        if (!Load()) return -1;
        // Re-enumerate now and then: adapters come and go (eGPU, driver updates, remote sessions).
        if (adapters.empty() || GetTickCount64() - enumeratedAt > 5 * 60 * 1000) Enumerate();
        int best = -1;
        bool failed = false;
        for (const auto& a : adapters) {
            KmtAdapterPerfData perf{};
            KmtQueryAdapterInfo q{ a.hAdapter, kKmtQaiAdapterPerfData, &perf, sizeof(perf) };
            if (query(&q) != 0) { failed = true; continue; }
            best = std::max(best, GpuTempFromDeciCelsius(perf.Temperature));  // integrated GPUs usually report 0
        }
        if (failed && best < 0) enumeratedAt = 0;  // handles went stale: enumerate again next time
        return best;
    }
};

// ---- CPU temperature: HWiNFO shared memory ---------------------------------------------------------------

#pragma pack(push, 1)
struct HwinfoHeader {
    DWORD signature;  // 'HWiS' while HWiNFO is publishing, 'DEAD' once it stopped
    DWORD version;
    DWORD revision;
    __int64 pollTime;
    DWORD sensorOffset, sensorSize, sensorCount;
    DWORD readingOffset, readingSize, readingCount;
};
struct HwinfoSensor {
    DWORD id, instance;
    char nameOrig[128];
    char nameUser[128];
};
struct HwinfoReading {
    DWORD type;  // 1 = temperature
    DWORD sensorIndex;
    DWORD readingId;
    char labelOrig[128];
    char labelUser[128];
    char unit[16];
    double value, valueMin, valueMax, valueAvg;
};
#pragma pack(pop)
constexpr DWORD kHwinfoSignature = 0x53695748;  // "HWiS"

std::wstring Widen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    if (!n) return L"";
    const int len = MultiByteToWideChar(CP_ACP, 0, s, static_cast<int>(n), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, static_cast<int>(n), out.data(), len);
    return out;
}

int HwinfoCpuTemp() {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    if (!map) map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"HWiNFO_SENS_SM2");
    if (!map) return -1;
    int result = -1;
    if (const auto* base = static_cast<const BYTE*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0))) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T size = VirtualQuery(base, &mbi, sizeof(mbi)) ? mbi.RegionSize : 0;
        const auto* h = reinterpret_cast<const HwinfoHeader*>(base);
        auto fits = [&](uint64_t off, uint64_t len) { return off + len <= size; };
        if (size >= sizeof(HwinfoHeader) && h->signature == kHwinfoSignature &&
            h->readingSize >= sizeof(HwinfoReading) && h->sensorSize >= sizeof(HwinfoSensor)) {
            std::vector<TempReading> temps;
            for (DWORD i = 0; i < h->readingCount; i++) {
                const uint64_t off = h->readingOffset + static_cast<uint64_t>(i) * h->readingSize;
                if (!fits(off, sizeof(HwinfoReading))) break;
                const auto* r = reinterpret_cast<const HwinfoReading*>(base + off);
                if (r->type != 1) continue;
                // Skip readings of sensors that are clearly not the processor (GPU, drives, ...).
                const uint64_t soff = h->sensorOffset + static_cast<uint64_t>(r->sensorIndex) * h->sensorSize;
                if (r->sensorIndex < h->sensorCount && fits(soff, sizeof(HwinfoSensor))) {
                    const std::wstring sensor = Lower(Widen(reinterpret_cast<const HwinfoSensor*>(base + soff)->nameOrig, 128));
                    if (sensor.find(L"gpu") != std::wstring::npos || sensor.find(L"drive") != std::wstring::npos ||
                        sensor.find(L"s.m.a.r.t") != std::wstring::npos || sensor.find(L"nvme") != std::wstring::npos)
                        continue;
                }
                double c = r->value;
                if (Lower(Widen(r->unit, 16)).find(L'f') != std::wstring::npos) c = (c - 32) * 5 / 9;  // set to Fahrenheit
                temps.push_back({ Widen(r->labelOrig, 128), c });
            }
            result = PickCpuTemp(temps);
        }
        UnmapViewOfFile(base);
    }
    CloseHandle(map);
    return result;
}

// ---- CPU temperature: LibreHardwareMonitor / OpenHardwareMonitor WMI --------------------------------------

struct WmiCpuTemp {
    IWbemServices* svc = nullptr;
    ULONGLONG retryAt = 0;
    ~WmiCpuTemp() { Release(); }
    void Release() {
        if (svc) svc->Release();
        svc = nullptr;
    }
    bool Connect() {
        if (svc) return true;
        if (GetTickCount64() < retryAt) return false;
        retryAt = GetTickCount64() + 30000;  // the monitor may be started later: try again every 30 s
        IWbemLocator* loc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                    reinterpret_cast<void**>(&loc))))
            return false;
        for (const wchar_t* ns : { L"ROOT\\LibreHardwareMonitor", L"ROOT\\OpenHardwareMonitor" }) {
            BSTR name = SysAllocString(ns);
            HRESULT hr = loc->ConnectServer(name, nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &svc);
            SysFreeString(name);
            if (SUCCEEDED(hr)) {
                CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                break;
            }
            svc = nullptr;
        }
        loc->Release();
        return svc != nullptr;
    }
    static std::wstring Str(IWbemClassObject* o, const wchar_t* prop) {
        VARIANT v;
        VariantInit(&v);
        std::wstring s;
        if (SUCCEEDED(o->Get(prop, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal) s = v.bstrVal;
        VariantClear(&v);
        return s;
    }
    int Sample() {
        if (!Connect()) return -1;
        BSTR lang = SysAllocString(L"WQL");
        BSTR wql = SysAllocString(L"SELECT Name, Value, Parent FROM Sensor WHERE SensorType = 'Temperature'");
        IEnumWbemClassObject* en = nullptr;
        HRESULT hr = svc->ExecQuery(lang, wql, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(wql);
        if (FAILED(hr) || !en) { Release(); return -1; }  // the monitor was closed: reconnect later
        std::vector<TempReading> temps;
        for (;;) {
            IWbemClassObject* o = nullptr;
            ULONG got = 0;
            if (en->Next(2000, 1, &o, &got) != WBEM_S_NO_ERROR || !got) break;
            // Parent is the hardware identifier: "/intelcpu/0", "/amdcpu/0", "/gpu-nvidia/0", "/lpc/nct6798d", ...
            const std::wstring parent = Lower(Str(o, L"Parent"));
            VARIANT v;
            VariantInit(&v);
            if (parent.find(L"cpu") != std::wstring::npos && SUCCEEDED(o->Get(L"Value", 0, &v, nullptr, nullptr))) {
                double c = v.vt == VT_R4 ? v.fltVal : v.vt == VT_R8 ? v.dblVal : -1;
                temps.push_back({ Str(o, L"Name"), c });
            }
            VariantClear(&v);
            o->Release();
        }
        en->Release();
        return PickCpuTemp(temps);
    }
};

// ---- Thread ----------------------------------------------------------------------------------------------

HWND g_notify = nullptr;
UINT g_msg = 0;
HANDLE g_thread = nullptr, g_stop = nullptr, g_wake = nullptr;
CRITICAL_SECTION g_lock;
Snapshot g_latest;
Wanted g_wanted;

bool Same(const Snapshot& a, const Snapshot& b) {
    return a.cpuPercent == b.cpuPercent && a.memPercent == b.memPercent && a.gpuPercent == b.gpuPercent &&
           a.cpuTempC == b.cpuTempC && a.gpuTempC == b.gpuTempC;
}

DWORD WINAPI Thread(LPVOID) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    {
        CpuSampler cpu;
        GpuLoadSampler gpuLoad;
        GpuTempSampler gpuTemp;
        WmiCpuTemp wmiTemp;
        HANDLE waits[2] = { g_stop, g_wake };
        for (;;) {
            EnterCriticalSection(&g_lock);
            const Wanted w = g_wanted;
            LeaveCriticalSection(&g_lock);
            if (!w.Any()) {
                gpuLoad.Close();
                wmiTemp.Release();
                cpu.primed = false;
                EnterCriticalSection(&g_lock);
                g_latest = Snapshot{};
                LeaveCriticalSection(&g_lock);
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
                continue;
            }
            Snapshot s;
            if (w.cpu) s.cpuPercent = cpu.Sample(); else cpu.primed = false;
            if (w.mem) s.memPercent = MemoryPercent();
            if (w.gpu) s.gpuPercent = gpuLoad.Sample(); else gpuLoad.Close();
            if (w.gpuTemp) s.gpuTempC = gpuTemp.Sample();
            if (w.cpuTemp) {
                s.cpuTempC = HwinfoCpuTemp();
                if (s.cpuTempC < 0 && com) s.cpuTempC = wmiTemp.Sample();
            } else {
                wmiTemp.Release();
            }
            EnterCriticalSection(&g_lock);
            const bool changed = !Same(s, g_latest);
            g_latest = s;
            LeaveCriticalSection(&g_lock);
            if (changed && g_notify) PostMessageW(g_notify, g_msg, 0, 0);
            if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) break;
        }
    }
    if (com) CoUninitialize();
    return 0;
}

}  // namespace

void Start(HWND notify, UINT msg) {
    if (g_thread) return;
    InitializeCriticalSection(&g_lock);
    g_notify = notify;
    g_msg = msg;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
}

void SetWanted(const Wanted& wanted) {
    if (!g_thread) return;
    EnterCriticalSection(&g_lock);
    g_wanted = wanted;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

Snapshot Latest() {
    if (!g_thread) return {};
    EnterCriticalSection(&g_lock);
    Snapshot s = g_latest;
    LeaveCriticalSection(&g_lock);
    return s;
}

void Stop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    g_thread = g_stop = g_wake = nullptr;
    g_notify = nullptr;
}

}  // namespace system_metrics
