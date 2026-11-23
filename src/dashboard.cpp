#include "dashboard.h"
#include "battery_intelligence.h"
#include "brands.h"
#include "alert_rules.h"
#include "device_aliases.h"
#include "i18n.h"

#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>

namespace dashboard {
namespace {

const wchar_t kDashboardClass[] = L"BatteryHubDashboardWindow";
HWND g_dashWnd = nullptr;

int g_selectedTab = 0; // 0: Overview, 1: Details, 2: Comparison, 3: Charging History
int g_selectedDeviceIndex = 0;
DeviceKind g_filterKind = DeviceKind::Other;

std::vector<DeviceStatus> g_devices;
battery_history::Store g_store;

// Theme palette (matching battery_history.cpp)
struct Palette {
    COLORREF bg, panel, card, border, grid, text, muted, blue, green, red, orange, activeTab, btnBg;
};
constexpr Palette kDark = {
    RGB(14, 18, 28), RGB(22, 30, 46), RGB(28, 38, 58), RGB(48, 64, 88), RGB(40, 52, 72),
    RGB(235, 242, 252), RGB(160, 178, 200), RGB(80, 160, 255), RGB(50, 210, 150),
    RGB(255, 110, 125), RGB(255, 170, 50), RGB(36, 52, 78), RGB(30, 42, 64)
};
constexpr Palette kLight = {
    RGB(244, 246, 250), RGB(255, 255, 255), RGB(248, 250, 253), RGB(212, 218, 228), RGB(226, 232, 240),
    RGB(20, 28, 40), RGB(95, 108, 125), RGB(0, 110, 210), RGB(18, 145, 68),
    RGB(210, 40, 30), RGB(215, 120, 10), RGB(225, 236, 252), RGB(236, 240, 246)
};

bool g_light = false;
bool AppsUseLightTheme() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}
const Palette& P() { return g_light ? kLight : kDark; }

HFONT g_fontRegular = nullptr;
HFONT g_fontBold = nullptr;
HFONT g_fontTitle = nullptr;
HFONT g_fontValue = nullptr;
HFONT g_fontSmall = nullptr;

int Ui(HWND w, int logical) {
    UINT dpi = w ? GetDpiForWindow(w) : 96;
    return MulDiv(logical, dpi ? static_cast<int>(dpi) : 96, 96);
}

void ApplyTheme(HWND w) {
    g_light = AppsUseLightTheme();
    HMODULE lib = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return;
    using AttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<AttributeFn>(GetProcAddress(lib, "DwmSetWindowAttribute"));
    if (fn) {
        BOOL dark = !g_light;
        if (FAILED(fn(w, 20, &dark, sizeof(dark)))) fn(w, 19, &dark, sizeof(dark));
    }
    FreeLibrary(lib);
}

HFONT CreateFontScaled(HWND w, int points, int weight) {
    HDC dc = GetDC(w);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(w, dc);
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void RoundedRect(HDC dc, const RECT& r, COLORREF fill, COLORREF outline, int radius = 10) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void Text(HDC dc, const std::wstring& str, RECT r, COLORREF color, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, str.c_str(), static_cast<int>(str.size()), &r, flags | DT_NOPREFIX);
}

// Sparkline helper: draws a compact battery history sparkline inside rect
void DrawSparkline(HDC dc, const RECT& rect, const std::vector<battery_history::Sample>& samples, COLORREF color) {
    if (samples.size() < 2) return;
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w <= 10 || h <= 10) return;

    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    size_t count = samples.size();
    for (size_t i = 1; i < count; ++i) {
        int x1 = rect.left + static_cast<int>((i - 1) * w / (count - 1));
        int y1 = rect.bottom - static_cast<int>(samples[i - 1].percent * h / 100);
        int x2 = rect.left + static_cast<int>(i * w / (count - 1));
        int y2 = rect.bottom - static_cast<int>(samples[i].percent * h / 100);
        y1 = std::clamp(y1, static_cast<int>(rect.top), static_cast<int>(rect.bottom));
        y2 = std::clamp(y2, static_cast<int>(rect.top), static_cast<int>(rect.bottom));
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Draw a battery progress bar
void DrawProgressBar(HDC dc, const RECT& r, int percent, COLORREF barColor, COLORREF trackColor) {
    RoundedRect(dc, r, trackColor, trackColor, 6);
    if (percent > 0) {
        int totalW = static_cast<int>(r.right - r.left);
        int fillW = std::clamp(totalW * percent / 100, 4, totalW);
        RECT fillRect{ r.left, r.top, r.left + fillW, r.bottom };
        RoundedRect(dc, fillRect, barColor, barColor, 6);
    }
}

std::wstring KindToString(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse: return L"Mouse";
        case DeviceKind::Headset: return L"Headset";
        case DeviceKind::Keyboard: return L"Keyboard";
        case DeviceKind::Gamepad: return L"Gamepad";
        case DeviceKind::Phone: return L"Phone";
        case DeviceKind::Microphone: return L"Microphone";
        default: return L"Device";
    }
}

std::wstring KindIcon(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse: return L"\U0001F5B1";      // Mouse
        case DeviceKind::Headset: return L"\U0001F3A7";    // Headset
        case DeviceKind::Keyboard: return L"\u2328";       // Keyboard
        case DeviceKind::Gamepad: return L"\U0001F3AE";    // Gamepad
        case DeviceKind::Phone: return L"\U0001F4F1";      // Phone
        case DeviceKind::Microphone: return L"\U0001F3A4"; // Mic
        default: return L"\U0001F50B";                    // Battery
    }
}

// ---------------- Paint View Tabs ----------------

void PaintOverview(HWND w, HDC dc, const RECT& area, int64_t now) {
    if (g_devices.empty()) {
        RECT r = area;
        SelectObject(dc, g_fontRegular);
        Text(dc, T(L"No devices connected or previously detected."), r, P().muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    const int pad = Ui(w, 14);
    const int cardW = Ui(w, 390);
    const int cardH = Ui(w, 175);
    const int cols = std::max<int>(1, static_cast<int>(area.right - area.left - pad) / (cardW + pad));

    for (size_t i = 0; i < g_devices.size(); ++i) {
        const auto& d = g_devices[i];
        int col = static_cast<int>(i % cols);
        int row = static_cast<int>(i / cols);

        int left = area.left + pad + col * (cardW + pad);
        int top = area.top + pad + row * (cardH + pad);
        if (top + cardH > area.bottom + 100) break; // clipped

        RECT cardRect{ left, top, left + cardW, top + cardH };
        bool isSelected = (static_cast<int>(i) == g_selectedDeviceIndex);
        COLORREF borderCol = isSelected ? P().blue : P().border;
        RoundedRect(dc, cardRect, P().panel, borderCol, Ui(w, 12));

        // Brand color indicator strip on left
        Accent acc = BrandOf(d);
        BrandColor bc = AccentColor(acc, g_light);
        COLORREF brandCol = acc != Accent::None ? RGB(bc.r, bc.g, bc.b) : P().blue;
        RECT brandBar{ left + Ui(w, 8), top + Ui(w, 12), left + Ui(w, 12), top + cardH - Ui(w, 12) };
        RoundedRect(dc, brandBar, brandCol, brandCol, 4);

        // Header: Kind icon + Name
        SelectObject(dc, g_fontBold);
        std::wstring title = KindIcon(d.kind) + L"  " + d.name;
        RECT nameRect{ left + Ui(w, 20), top + Ui(w, 12), left + cardW - Ui(w, 100), top + Ui(w, 36) };
        Text(dc, title, nameRect, P().text);

        // Status badge on top right
        std::wstring statusStr;
        COLORREF statusColor = P().muted;
        if (!d.connected) {
            statusStr = T(L"Disconnected");
            statusColor = P().muted;
        } else if (d.full || (d.charging && d.percent >= 100)) {
            statusStr = T(L"Fully charged ✓");
            statusColor = P().green;
        } else if (d.charging) {
            statusStr = T(L"Charging ⚡");
            statusColor = P().blue;
        } else if (d.asleep) {
            statusStr = T(L"Sleeping");
            statusColor = P().orange;
        } else if (d.percent >= 0 && d.percent <= 20) {
            statusStr = T(L"Low battery");
            statusColor = P().red;
        } else if (d.percent >= 0) {
            statusStr = T(L"Connected");
            statusColor = P().green;
        } else {
            statusStr = T(L"Reading unavailable");
            statusColor = P().muted;
        }

        SelectObject(dc, g_fontSmall);
        RECT badgeRect{ left + cardW - Ui(w, 130), top + Ui(w, 12), left + cardW - Ui(w, 14), top + Ui(w, 34) };
        Text(dc, statusStr, badgeRect, statusColor, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        // Subtitle: Connection type (via)
        SelectObject(dc, g_fontSmall);
        RECT viaRect{ left + Ui(w, 20), top + Ui(w, 36), left + cardW - Ui(w, 20), top + Ui(w, 52) };
        Text(dc, d.via, viaRect, P().muted);

        // Battery level display
        int pct = d.percent;
        COLORREF pctColor = (!d.charging && pct >= 0 && pct <= 20) ? P().red : (d.charging ? P().blue : P().text);
        SelectObject(dc, g_fontValue);
        std::wstring pctStr = pct >= 0 ? (std::to_wstring(pct) + L"%") : L"—";
        RECT pctRect{ left + Ui(w, 20), top + Ui(w, 58), left + Ui(w, 100), top + Ui(w, 94) };
        Text(dc, pctStr, pctRect, pctColor);

        // Progress bar
        RECT barRect{ left + Ui(w, 105), top + Ui(w, 70), left + cardW - Ui(w, 110), top + Ui(w, 82) };
        COLORREF barFill = d.charging ? P().blue : (pct <= 20 ? P().red : (pct <= 35 ? P().orange : P().green));
        DrawProgressBar(dc, barRect, std::max(0, pct), barFill, P().grid);

        // Sparkline history graph on right
        auto samples = g_store.For(d.key, now - 86400 * 7, now);
        RECT sparkRect{ left + cardW - Ui(w, 100), top + Ui(w, 58), left + cardW - Ui(w, 16), top + Ui(w, 92) };
        DrawSparkline(dc, sparkRect, samples, barFill);

        // Multi-battery component line if available
        int infoY = top + Ui(w, 102);
        if (!d.batteries.empty()) {
            SelectObject(dc, g_fontSmall);
            std::wstring compStr;
            for (size_t b = 0; b < d.batteries.size(); ++b) {
                if (b > 0) compStr += L"   ";
                compStr += d.batteries[b].name + L": " +
                           (d.batteries[b].percent >= 0 ? (std::to_wstring(d.batteries[b].percent) + L"%") : L"—") +
                           (d.batteries[b].charging ? L" ⚡" : L"");
            }
            RECT compRect{ left + Ui(w, 20), infoY, left + cardW - Ui(w, 20), infoY + Ui(w, 20) };
            Text(dc, compStr, compRect, P().text);
            infoY += Ui(w, 22);
        }

        // Drain rate & remaining runtime summary
        auto drain = battery_intelligence::CalculateDrainRate(d.key, d.name, samples, d.percent, d.charging, now);
        std::wstring statsLine;
        if (drain.hasData) {
            statsLine = TF(L"Drain: {rate} · Remaining: ~{hrs} h", {
                { L"rate", drain.rateLabel },
                { L"hrs", std::to_wstring(static_cast<int>(std::round(drain.hoursRemaining))) }
            });
        } else if (d.charging) {
            statsLine = T(L"Currently charging · Runtime estimate paused");
        } else {
            statsLine = TF(L"Drain: {reason}", { { L"reason", drain.statusReason } });
        }

        SelectObject(dc, g_fontSmall);
        RECT statsRect{ left + Ui(w, 20), infoY, left + cardW - Ui(w, 20), infoY + Ui(w, 22) };
        Text(dc, statsLine, statsRect, P().muted);

        // Click prompt
        RECT clickRect{ left + Ui(w, 20), top + cardH - Ui(w, 24), left + cardW - Ui(w, 20), top + cardH - Ui(w, 6) };
        Text(dc, T(L"Click card for full device details →"), clickRect, P().blue, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
}

void PaintDetails(HWND w, HDC dc, const RECT& area, int64_t now) {
    if (g_devices.empty() || g_selectedDeviceIndex >= static_cast<int>(g_devices.size())) {
        RECT r = area;
        SelectObject(dc, g_fontRegular);
        Text(dc, T(L"No device selected."), r, P().muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    const auto& d = g_devices[g_selectedDeviceIndex];
    auto samples = g_store.For(d.key, now - 86400 * 30, now);
    auto drain = battery_intelligence::CalculateDrainRate(d.key, d.name, samples, d.percent, d.charging, now);
    auto fullRun = battery_intelligence::EstimateFullChargeRuntime(d.key, samples, now);
    auto health = battery_intelligence::GetBatteryHealth(d.key, d);
    auto chargeList = battery_intelligence::GetChargingHistory(d.key);

    const int pad = Ui(w, 18);
    int y = area.top + pad;

    // Header Card: Device Identity & Status
    RECT headCard{ area.left + pad, y, area.right - pad, y + Ui(w, 100) };
    RoundedRect(dc, headCard, P().panel, P().border, Ui(w, 12));

    SelectObject(dc, g_fontTitle);
    std::wstring headTitle = KindIcon(d.kind) + L"  " + d.name + L"  (" + KindToString(d.kind) + L")";
    RECT nameRect{ headCard.left + Ui(w, 18), y + Ui(w, 14), headCard.right - Ui(w, 160), y + Ui(w, 46) };
    Text(dc, headTitle, nameRect, P().text);

    SelectObject(dc, g_fontRegular);
    std::wstring sub = d.via + L"  •  " + (d.connected ? T(L"Connected") : T(L"Disconnected")) +
                       (d.charging ? (L"  •  " + std::wstring(T(L"Charging ⚡"))) : L"");
    RECT subRect{ headCard.left + Ui(w, 18), y + Ui(w, 50), headCard.right - Ui(w, 160), y + Ui(w, 75) };
    Text(dc, sub, subRect, P().muted);

    // Large battery %
    SelectObject(dc, g_fontTitle);
    std::wstring pctStr = d.percent >= 0 ? (std::to_wstring(d.percent) + L"%") : L"—";
    COLORREF pctColor = d.charging ? P().blue : (d.percent <= 20 ? P().red : P().text);
    RECT bigPctRect{ headCard.right - Ui(w, 140), y + Ui(w, 14), headCard.right - Ui(w, 18), y + Ui(w, 75) };
    Text(dc, pctStr, bigPctRect, pctColor, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    y += Ui(w, 112);

    // Two-column layout for details
    int colW = (area.right - area.left - 3 * pad) / 2;

    // Left Column: Intelligence, Drain Rate, Runtime & Health
    int leftX = area.left + pad;
    int leftY = y;

    // 1. Drain Rate Card
    RECT drainCard{ leftX, leftY, leftX + colW, leftY + Ui(w, 130) };
    RoundedRect(dc, drainCard, P().panel, P().border, Ui(w, 12));
    SelectObject(dc, g_fontBold);
    Text(dc, T(L"Battery Drain Rate"), RECT{ drainCard.left + Ui(w, 14), leftY + Ui(w, 10), drainCard.right - Ui(w, 14), leftY + Ui(w, 32) }, P().muted);

    SelectObject(dc, g_fontRegular);
    if (drain.hasData) {
        SelectObject(dc, g_fontValue);
        Text(dc, drain.rateLabel, RECT{ drainCard.left + Ui(w, 14), leftY + Ui(w, 32), drainCard.right - Ui(w, 14), leftY + Ui(w, 62) }, P().blue);
        SelectObject(dc, g_fontSmall);
        Text(dc, drain.summaryMessage, RECT{ drainCard.left + Ui(w, 14), leftY + Ui(w, 64), drainCard.right - Ui(w, 14), leftY + Ui(w, 118) }, P().text, DT_LEFT | DT_WORDBREAK);
    } else {
        SelectObject(dc, g_fontRegular);
        std::wstring msg = drain.statusReason.empty() ? T(L"Insufficient reliable history.") : drain.statusReason;
        Text(dc, msg, RECT{ drainCard.left + Ui(w, 14), leftY + Ui(w, 40), drainCard.right - Ui(w, 14), leftY + Ui(w, 80) }, P().muted);
    }
    leftY += Ui(w, 142);

    // 2. Full-Charge Runtime & Battery Health Card
    RECT runtimeCard{ leftX, leftY, leftX + colW, leftY + Ui(w, 145) };
    RoundedRect(dc, runtimeCard, P().panel, P().border, Ui(w, 12));
    SelectObject(dc, g_fontBold);
    Text(dc, T(L"Full-Charge Runtime & Battery Health"), RECT{ runtimeCard.left + Ui(w, 14), leftY + Ui(w, 10), runtimeCard.right - Ui(w, 14), leftY + Ui(w, 32) }, P().muted);

    SelectObject(dc, g_fontRegular);
    std::wstring runStr = fullRun.hasEstimate ? fullRun.displayLabel : (std::wstring(T(L"Estimated full-charge runtime: ")) + T(L"Collecting data"));
    Text(dc, runStr, RECT{ runtimeCard.left + Ui(w, 14), leftY + Ui(w, 36), runtimeCard.right - Ui(w, 14), leftY + Ui(w, 58) }, P().text);

    if (fullRun.hasEstimate) {
        SelectObject(dc, g_fontSmall);
        std::wstring sessDetail = fullRun.recentLabel + L"  •  " + fullRun.averageLabel;
        Text(dc, sessDetail, RECT{ runtimeCard.left + Ui(w, 14), leftY + Ui(w, 60), runtimeCard.right - Ui(w, 14), leftY + Ui(w, 80) }, P().muted);
    }

    // Health line
    SelectObject(dc, g_fontRegular);
    std::wstring healthStr = TF(L"Battery Health: {status}", { { L"status", health.displayText } });
    if (health.supported && health.designCapacityMwh > 0) {
        healthStr += TF(L" ({full}/{design} mWh)", {
            { L"full", std::to_wstring(health.fullChargeCapacityMwh) },
            { L"design", std::to_wstring(health.designCapacityMwh) }
        });
    }
    Text(dc, healthStr, RECT{ runtimeCard.left + Ui(w, 14), leftY + Ui(w, 86), runtimeCard.right - Ui(w, 14), leftY + Ui(w, 110) },
         health.supported ? P().green : P().muted);

    SelectObject(dc, g_fontSmall);
    Text(dc, T(L"* Health tracking applies only when hardware reports valid capacity data."),
         RECT{ runtimeCard.left + Ui(w, 14), leftY + Ui(w, 112), runtimeCard.right - Ui(w, 14), leftY + Ui(w, 136) }, P().muted);

    leftY += Ui(w, 157);

    // Multi-battery component card if applicable
    if (!d.batteries.empty()) {
        RECT compCard{ leftX, leftY, leftX + colW, leftY + Ui(w, 100) };
        RoundedRect(dc, compCard, P().panel, P().border, Ui(w, 12));
        SelectObject(dc, g_fontBold);
        Text(dc, T(L"Independent Battery Components"), RECT{ compCard.left + Ui(w, 14), leftY + Ui(w, 10), compCard.right - Ui(w, 14), leftY + Ui(w, 32) }, P().muted);

        int cy = leftY + Ui(w, 38);
        SelectObject(dc, g_fontRegular);
        for (const auto& b : d.batteries) {
            std::wstring compLine = b.name + L": " + (b.percent >= 0 ? (std::to_wstring(b.percent) + L"%") : L"—") +
                                    (b.charging ? L" (Charging ⚡)" : L"");
            Text(dc, compLine, RECT{ compCard.left + Ui(w, 16), cy, compCard.right - Ui(w, 16), cy + Ui(w, 22) }, P().text);
            cy += Ui(w, 22);
        }
    }

    // Right Column: Battery History & Charging Log
    int rightX = leftX + colW + pad;
    int rightY = y;

    // 1. History Chart Card
    RECT chartCard{ rightX, rightY, rightX + colW, rightY + Ui(w, 190) };
    RoundedRect(dc, chartCard, P().panel, P().border, Ui(w, 12));
    SelectObject(dc, g_fontBold);
    Text(dc, T(L"Battery History Trend (Last 7 Days)"), RECT{ chartCard.left + Ui(w, 14), rightY + Ui(w, 10), chartCard.right - Ui(w, 14), rightY + Ui(w, 32) }, P().muted);

    RECT plotArea{ chartCard.left + Ui(w, 14), rightY + Ui(w, 40), chartCard.right - Ui(w, 14), chartCard.bottom - Ui(w, 30) };
    RoundedRect(dc, plotArea, P().card, P().grid, 6);
    DrawSparkline(dc, plotArea, samples, P().blue);

    SelectObject(dc, g_fontSmall);
    Text(dc, T(L"7 days ago"), RECT{ plotArea.left, chartCard.bottom - Ui(w, 24), plotArea.left + Ui(w, 100), chartCard.bottom - Ui(w, 4) }, P().muted);
    Text(dc, T(L"Now"), RECT{ plotArea.right - Ui(w, 60), chartCard.bottom - Ui(w, 24), plotArea.right, chartCard.bottom - Ui(w, 4) }, P().muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    rightY += Ui(w, 202);

    // 2. Charging Sessions for this Device
    RECT chgCard{ rightX, rightY, rightX + colW, rightY + Ui(w, 180) };
    RoundedRect(dc, chgCard, P().panel, P().border, Ui(w, 12));
    SelectObject(dc, g_fontBold);
    Text(dc, T(L"Recent Charging Sessions"), RECT{ chgCard.left + Ui(w, 14), rightY + Ui(w, 10), chgCard.right - Ui(w, 14), rightY + Ui(w, 32) }, P().muted);

    SelectObject(dc, g_fontSmall);
    int rowY = rightY + Ui(w, 36);
    if (chargeList.empty()) {
        Text(dc, T(L"No charging sessions recorded yet."), RECT{ chgCard.left + Ui(w, 14), rowY, chgCard.right - Ui(w, 14), rowY + Ui(w, 25) }, P().muted);
    } else {
        // Table header
        Text(dc, L"Date", RECT{ chgCard.left + Ui(w, 14), rowY, chgCard.left + Ui(w, 90), rowY + Ui(w, 20) }, P().muted);
        Text(dc, L"Start → End", RECT{ chgCard.left + Ui(w, 95), rowY, chgCard.left + Ui(w, 190), rowY + Ui(w, 20) }, P().muted);
        Text(dc, L"Duration", RECT{ chgCard.left + Ui(w, 195), rowY, chgCard.left + Ui(w, 270), rowY + Ui(w, 20) }, P().muted);
        Text(dc, L"Status", RECT{ chgCard.left + Ui(w, 275), rowY, chgCard.right - Ui(w, 14), rowY + Ui(w, 20) }, P().muted);
        rowY += Ui(w, 22);

        size_t showCount = std::min<size_t>(chargeList.size(), 4);
        for (size_t s = 0; s < showCount; ++s) {
            const auto& sess = chargeList[s];
            std::time_t t = static_cast<std::time_t>(sess.startTime);
            std::tm tmVal{};
            localtime_s(&tmVal, &t);
            wchar_t dateBuf[32]{};
            wcsftime(dateBuf, 32, L"%b %d", &tmVal);

            std::wstring range = std::to_wstring(sess.startPercent) + L"% → " + std::to_wstring(sess.endPercent) + L"%";
            std::wstring dur = std::to_wstring(sess.durationMinutes) + L"m";
            std::wstring status = sess.completed ? L"Completed ✓" : L"Interrupted";

            Text(dc, dateBuf, RECT{ chgCard.left + Ui(w, 14), rowY, chgCard.left + Ui(w, 90), rowY + Ui(w, 20) }, P().text);
            Text(dc, range, RECT{ chgCard.left + Ui(w, 95), rowY, chgCard.left + Ui(w, 190), rowY + Ui(w, 20) }, P().text);
            Text(dc, dur, RECT{ chgCard.left + Ui(w, 195), rowY, chgCard.left + Ui(w, 270), rowY + Ui(w, 20) }, P().text);
            Text(dc, status, RECT{ chgCard.left + Ui(w, 275), rowY, chgCard.right - Ui(w, 14), rowY + Ui(w, 20) }, sess.completed ? P().green : P().muted);
            rowY += Ui(w, 22);
        }
    }
}

void PaintComparison(HWND w, HDC dc, const RECT& area, int64_t now) {
    const int pad = Ui(w, 18);
    int y = area.top + pad;

    SelectObject(dc, g_fontTitle);
    Text(dc, T(L"Battery Consumption Comparison"), RECT{ area.left + pad, y, area.right - pad, y + Ui(w, 36) }, P().text);

    SelectObject(dc, g_fontSmall);
    Text(dc, T(L"Compare battery consumption across devices over comparable observation periods."),
         RECT{ area.left + pad, y + Ui(w, 34), area.right - pad, y + Ui(w, 54) }, P().muted);

    y += Ui(w, 64);

    auto list = battery_intelligence::CompareConsumption(g_devices, g_store, now, g_filterKind);

    // Disclaimer alert box
    RECT alertBox{ area.left + pad, y, area.right - pad, y + Ui(w, 48) };
    RoundedRect(dc, alertBox, P().card, P().border, 8);
    SelectObject(dc, g_fontSmall);
    Text(dc, T(L"Notice: Comparisons are percentage-based (%/day). Devices with larger battery capacities may consume more energy (mWh) per percentage point."),
         RECT{ alertBox.left + Ui(w, 12), alertBox.top + Ui(w, 6), alertBox.right - Ui(w, 12), alertBox.bottom - Ui(w, 6) }, P().muted, DT_LEFT | DT_WORDBREAK);

    y += Ui(w, 62);

    // Comparison Table
    RECT tableHead{ area.left + pad, y, area.right - pad, y + Ui(w, 30) };
    RoundedRect(dc, tableHead, P().panel, P().border, 6);
    SelectObject(dc, g_fontBold);
    Text(dc, L"Device", RECT{ tableHead.left + Ui(w, 16), y, tableHead.left + Ui(w, 240), y + Ui(w, 30) }, P().muted);
    Text(dc, L"Category", RECT{ tableHead.left + Ui(w, 245), y, tableHead.left + Ui(w, 350), y + Ui(w, 30) }, P().muted);
    Text(dc, L"Drain Rate", RECT{ tableHead.left + Ui(w, 355), y, tableHead.left + Ui(w, 500), y + Ui(w, 30) }, P().muted);
    Text(dc, L"Est. Full Runtime", RECT{ tableHead.left + Ui(w, 505), y, tableHead.right - Ui(w, 16), y + Ui(w, 30) }, P().muted);

    y += Ui(w, 36);

    SelectObject(dc, g_fontRegular);
    for (const auto& item : list) {
        RECT rowRect{ area.left + pad, y, area.right - pad, y + Ui(w, 42) };
        RoundedRect(dc, rowRect, P().panel, P().grid, 6);

        Text(dc, item.name, RECT{ rowRect.left + Ui(w, 16), y, rowRect.left + Ui(w, 240), y + Ui(w, 42) }, P().text);
        Text(dc, KindToString(item.kind), RECT{ rowRect.left + Ui(w, 245), y, rowRect.left + Ui(w, 350), y + Ui(w, 42) }, P().muted);
        Text(dc, item.rateText, RECT{ rowRect.left + Ui(w, 355), y, rowRect.left + Ui(w, 500), y + Ui(w, 42) }, item.hasData ? P().blue : P().muted);
        Text(dc, item.runtimeText, RECT{ rowRect.left + Ui(w, 505), y, rowRect.right - Ui(w, 16), y + Ui(w, 42) }, item.hasData ? P().green : P().muted);

        y += Ui(w, 48);
    }
}

void PaintChargingHistory(HWND w, HDC dc, const RECT& area) {
    const int pad = Ui(w, 18);
    int y = area.top + pad;

    SelectObject(dc, g_fontTitle);
    Text(dc, T(L"Charging Session History"), RECT{ area.left + pad, y, area.right - pad, y + Ui(w, 36) }, P().text);

    SelectObject(dc, g_fontSmall);
    Text(dc, T(L"Locally recorded charging sessions across all wireless devices."),
         RECT{ area.left + pad, y + Ui(w, 34), area.right - pad, y + Ui(w, 54) }, P().muted);

    y += Ui(w, 64);

    auto sessions = battery_intelligence::GetAllChargingHistory();

    // Table Header
    RECT tableHead{ area.left + pad, y, area.right - pad, y + Ui(w, 32) };
    RoundedRect(dc, tableHead, P().panel, P().border, 6);
    SelectObject(dc, g_fontBold);
    Text(dc, L"Date & Time", RECT{ tableHead.left + Ui(w, 16), y, tableHead.left + Ui(w, 150), y + Ui(w, 32) }, P().muted);
    Text(dc, L"Device", RECT{ tableHead.left + Ui(w, 155), y, tableHead.left + Ui(w, 380), y + Ui(w, 32) }, P().muted);
    Text(dc, L"Start → End Level", RECT{ tableHead.left + Ui(w, 385), y, tableHead.left + Ui(w, 520), y + Ui(w, 32) }, P().muted);
    Text(dc, L"Duration", RECT{ tableHead.left + Ui(w, 525), y, tableHead.left + Ui(w, 620), y + Ui(w, 32) }, P().muted);
    Text(dc, L"Outcome", RECT{ tableHead.left + Ui(w, 625), y, tableHead.right - Ui(w, 16), y + Ui(w, 32) }, P().muted);

    y += Ui(w, 38);

    if (sessions.empty()) {
        SelectObject(dc, g_fontRegular);
        RECT noRec{ area.left + pad, y + Ui(w, 20), area.right - pad, y + Ui(w, 60) };
        Text(dc, T(L"No charging sessions recorded yet. Plug in a device to record history."), noRec, P().muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    SelectObject(dc, g_fontRegular);
    size_t limit = std::min<size_t>(sessions.size(), 12);
    for (size_t i = 0; i < limit; ++i) {
        const auto& s = sessions[i];
        RECT rowRect{ area.left + pad, y, area.right - pad, y + Ui(w, 36) };
        RoundedRect(dc, rowRect, P().panel, P().grid, 6);

        std::time_t t = static_cast<std::time_t>(s.startTime);
        std::tm tmVal{};
        localtime_s(&tmVal, &t);
        wchar_t dateBuf[64]{};
        wcsftime(dateBuf, 64, L"%Y-%m-%d %H:%M", &tmVal);

        std::wstring levelStr = std::to_wstring(s.startPercent) + L"% → " + std::to_wstring(s.endPercent) + L"% (+" +
                                std::to_wstring(s.endPercent - s.startPercent) + L"%)";
        std::wstring durStr = std::to_wstring(s.durationMinutes) + L" min";
        std::wstring outStr = s.completed ? L"Completed ✓" : L"Interrupted";

        Text(dc, dateBuf, RECT{ rowRect.left + Ui(w, 16), y, rowRect.left + Ui(w, 150), y + Ui(w, 36) }, P().muted);
        Text(dc, s.deviceName, RECT{ rowRect.left + Ui(w, 155), y, rowRect.left + Ui(w, 380), y + Ui(w, 36) }, P().text);
        Text(dc, levelStr, RECT{ rowRect.left + Ui(w, 385), y, rowRect.left + Ui(w, 520), y + Ui(w, 36) }, P().text);
        Text(dc, durStr, RECT{ rowRect.left + Ui(w, 525), y, rowRect.left + Ui(w, 620), y + Ui(w, 36) }, P().text);
        Text(dc, outStr, RECT{ rowRect.left + Ui(w, 625), y, rowRect.right - Ui(w, 16), y + Ui(w, 36) }, s.completed ? P().green : P().orange);

        y += Ui(w, 42);
    }
}

// ---------------- Main Paint & WndProc ----------------

void PaintDashboard(HWND w) {
    PAINTSTRUCT ps{};
    HDC screen = BeginPaint(w, &ps);
    RECT bounds{};
    GetClientRect(w, &bounds);
    int width = bounds.right, height = bounds.bottom;
    if (width <= 0 || height <= 0) { EndPaint(w, &ps); return; }

    HDC memDC = CreateCompatibleDC(screen);
    HBITMAP memBitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ oldBmp = SelectObject(memDC, memBitmap);

    // Background fill
    HBRUSH bgBrush = CreateSolidBrush(P().bg);
    FillRect(memDC, &bounds, bgBrush);
    DeleteObject(bgBrush);

    int navH = Ui(w, 60);

    // Top Navigation Bar
    RECT navRect{ 0, 0, width, navH };
    RoundedRect(memDC, navRect, P().panel, P().border, 0);

    // App Title on Top-Left
    SelectObject(memDC, g_fontTitle);
    Text(memDC, L"BatteryHub  •  Dashboard", RECT{ Ui(w, 20), 0, Ui(w, 300), navH }, P().text);

    // Tab buttons in navigation bar
    const wchar_t* tabTitles[] = { L"Overview", L"Device Details", L"Battery Comparison", L"Charging History" };
    int tabX = Ui(w, 320);
    int tabW = Ui(w, 150);
    int tabH = Ui(w, 36);
    int tabY = (navH - tabH) / 2;

    for (int i = 0; i < 4; ++i) {
        RECT tabR{ tabX, tabY, tabX + tabW, tabY + tabH };
        bool isActive = (i == g_selectedTab);
        COLORREF tabBg = isActive ? P().activeTab : P().btnBg;
        COLORREF tabBorder = isActive ? P().blue : P().border;
        RoundedRect(memDC, tabR, tabBg, tabBorder, 8);
        SelectObject(memDC, isActive ? g_fontBold : g_fontRegular);
        Text(memDC, tabTitles[i], tabR, isActive ? P().blue : P().text, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        tabX += tabW + Ui(w, 10);
    }

    // View Content Area
    RECT contentArea{ 0, navH, width, height };
    int64_t now = static_cast<int64_t>(std::time(nullptr));

    switch (g_selectedTab) {
        case 0: PaintOverview(w, memDC, contentArea, now); break;
        case 1: PaintDetails(w, memDC, contentArea, now); break;
        case 2: PaintComparison(w, memDC, contentArea, now); break;
        case 3: PaintChargingHistory(w, memDC, contentArea); break;
    }

    BitBlt(screen, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    EndPaint(w, &ps);
}

void OnClick(HWND w, int x, int y) {
    int navH = Ui(w, 60);
    if (y < navH) {
        // Tab clicks
        int tabX = Ui(w, 320);
        int tabW = Ui(w, 150);
        int tabH = Ui(w, 36);
        int tabY = (navH - tabH) / 2;

        for (int i = 0; i < 4; ++i) {
            RECT tabR{ tabX, tabY, tabX + tabW, tabY + tabH };
            if (x >= tabR.left && x <= tabR.right && y >= tabR.top && y <= tabR.bottom) {
                g_selectedTab = i;
                InvalidateRect(w, nullptr, FALSE);
                return;
            }
            tabX += tabW + Ui(w, 10);
        }
    } else if (g_selectedTab == 0) {
        // Overview card click: select device and open Details tab
        const int pad = Ui(w, 14);
        const int cardW = Ui(w, 390);
        const int cardH = Ui(w, 175);
        RECT client{};
        GetClientRect(w, &client);
        const int cols = std::max<int>(1, static_cast<int>(client.right - 2 * pad) / (cardW + pad));

        for (size_t i = 0; i < g_devices.size(); ++i) {
            int col = static_cast<int>(i % cols);
            int row = static_cast<int>(i / cols);
            int left = pad + col * (cardW + pad);
            int top = navH + pad + row * (cardH + pad);

            if (x >= left && x <= left + cardW && y >= top && y <= top + cardH) {
                g_selectedDeviceIndex = static_cast<int>(i);
                g_selectedTab = 1; // Switch to Details
                InvalidateRect(w, nullptr, FALSE);
                return;
            }
        }
    }
}

LRESULT CALLBACK DashboardProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            ApplyTheme(w);
            g_fontRegular = CreateFontScaled(w, 10, FW_NORMAL);
            g_fontBold = CreateFontScaled(w, 10, FW_SEMIBOLD);
            g_fontTitle = CreateFontScaled(w, 14, FW_SEMIBOLD);
            g_fontValue = CreateFontScaled(w, 16, FW_BOLD);
            g_fontSmall = CreateFontScaled(w, 9, FW_NORMAL);
            return 0;
        }
        case WM_SETTINGCHANGE: {
            if (lp && !lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet")) {
                ApplyTheme(w);
                InvalidateRect(w, nullptr, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp);
            int y = HIWORD(lp);
            OnClick(w, x, y);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            if (info) {
                info->ptMinTrackSize.x = Ui(w, 820);
                info->ptMinTrackSize.y = Ui(w, 580);
            }
            return 0;
        }
        case WM_SIZE: {
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PaintDashboard(w);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: {
            DestroyWindow(w);
            return 0;
        }
        case WM_DESTROY: {
            g_dashWnd = nullptr;
            if (g_fontRegular) DeleteObject(g_fontRegular);
            if (g_fontBold) DeleteObject(g_fontBold);
            if (g_fontTitle) DeleteObject(g_fontTitle);
            if (g_fontValue) DeleteObject(g_fontValue);
            if (g_fontSmall) DeleteObject(g_fontSmall);
            g_fontRegular = g_fontBold = g_fontTitle = g_fontValue = g_fontSmall = nullptr;
            return 0;
        }
    }
    return DefWindowProcW(w, msg, wp, lp);
}

} // namespace

void Show(HWND owner) {
    if (g_dashWnd && IsWindow(g_dashWnd)) {
        ShowWindow(g_dashWnd, SW_RESTORE);
        SetForegroundWindow(g_dashWnd);
        InvalidateRect(g_dashWnd, nullptr, TRUE);
        return;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = DashboardProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kDashboardClass;
    RegisterClassExW(&wc);

    RECT bounds{ 0, 0, 960, 680 };
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);

    g_dashWnd = CreateWindowExW(WS_EX_APPWINDOW, kDashboardClass, L"BatteryHub Dashboard",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                bounds.right - bounds.left, bounds.bottom - bounds.top,
                                owner, nullptr, inst, nullptr);

    if (!g_dashWnd) return;
    ShowWindow(g_dashWnd, SW_SHOWNORMAL);
    UpdateWindow(g_dashWnd);
    SetForegroundWindow(g_dashWnd);
}

void Close() {
    if (g_dashWnd && IsWindow(g_dashWnd)) {
        DestroyWindow(g_dashWnd);
    }
}

bool IsOpen() {
    return g_dashWnd && IsWindow(g_dashWnd);
}

void UpdateDevices(const std::vector<DeviceStatus>& devices, const battery_history::Store& store) {
    g_devices = devices;
    g_store = store;
    if (g_selectedDeviceIndex >= static_cast<int>(g_devices.size())) {
        g_selectedDeviceIndex = 0;
    }
    if (g_dashWnd && IsWindow(g_dashWnd)) {
        InvalidateRect(g_dashWnd, nullptr, FALSE);
    }
}

void SelectDevice(const std::wstring& key) {
    for (size_t i = 0; i < g_devices.size(); ++i) {
        if (g_devices[i].key == key) {
            g_selectedDeviceIndex = static_cast<int>(i);
            g_selectedTab = 1; // Details tab
            if (g_dashWnd && IsWindow(g_dashWnd)) {
                InvalidateRect(g_dashWnd, nullptr, FALSE);
            }
            break;
        }
    }
}

} // namespace dashboard
