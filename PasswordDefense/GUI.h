
//GUI.h
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#define LOADER_BRAND "++"
#define WINDOW_WIDTH  785
#define WINDOW_HEIGHT 695

#include <string>
#include <thread>
#include <iostream>
#include <random>

namespace render
{
	// FUNCTIONS DECLARATIONS
	void GUIInit();
	void GUIShutDown();

	// UI STYLES
	void setChildStyle();
	void endChildStyle();
	void setCheckboxStyle();
	void endCheckboxStyle();
	float BeginCenteredColumn(const char* id, float maxW);
	void EndCenteredColumn();

	// Soft container: draws lifted surface with shadow behind content
	void BeginSoftContainer(float padding = 24.0f, float rounding = 12.0f);
	void EndSoftContainer();

	// APPLICATION
	void DisplayProgram();

	// THEME
	void ApplyDarkTheme();
	void ApplyLightTheme();
	void ApplyTheme(bool dark);

#ifdef __APPLE__
	// macOS window helpers
	void SetAlwaysOnTop(bool enabled);

	// macOS menu bar status item (system tray equivalent)
	void SetMinimizeToTray(bool enabled);
	bool ConsumeTrayLock();
	bool ConsumeTrayQuit();
	void RemoveTrayIcon();
	void ShowWindow();
	void HideWindow();
	bool IsWindowVisible();
	void RequestAppQuit();
	bool IsMinimizeToTrayEnabled();
#endif

	// FONTS
	extern ImFont* Font15;
	extern ImFont* FontIcons;
	extern ImFont* FontSmall;   // 12px - captions, subtitles
	extern ImFont* FontRegular; // 15px - body text, labels
	extern ImFont* FontBold;    // 15px - bold labels
	extern ImFont* FontLarge;   // 18px - titles, headers
}
