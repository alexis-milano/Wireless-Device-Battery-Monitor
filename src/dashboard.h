#pragma once
// Optional Native Win32 Device Dashboard for BatteryHub.
// Provides device cards, detailed analytics, battery intelligence,
// charging history, and device battery consumption comparisons.
#include <windows.h>
#include <string>
#include <vector>

#include "devices.h"
#include "history_model.h"

namespace dashboard {

// Shows or restores the dashboard window.
void Show(HWND owner = nullptr);

// Closes the dashboard window if open.
void Close();

// Returns true if the dashboard window is currently open.
bool IsOpen();

// Updates live device data when new poll results arrive.
void UpdateDevices(const std::vector<DeviceStatus>& devices, const battery_history::Store& store);

// Selects a specific device by key and switches to the Details tab.
void SelectDevice(const std::wstring& key);

} // namespace dashboard
