// GUI.mm — macOS port (Metal backend)
// Keeps: fonts, themes, style helpers, soft container
// Removed: D3D11, Win32, system tray, WndProc, window creation

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#include "GUI.h"
#include "UI.h"
// TODO: switch back to "save.h" once save.cpp is ported
#include "save.h"
#include "IconsMaterialDesignIcons.h"
#include "mdi_font_data.h"

namespace render
{
	// FONTS
	ImFont* Font15 = nullptr;
	ImFont* FontIcons = nullptr;
	ImFont* FontSmall = nullptr;
	ImFont* FontRegular = nullptr;
	ImFont* FontBold = nullptr;
	ImFont* FontLarge = nullptr;
}

// ============================================================
// UI STYLE HELPERS (cross-platform, pure ImGui)
// ============================================================

void render::setChildStyle()
{
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 11));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 2));

	const ImU32 childBg = cfg::is_dark_theme()
		? IM_COL32(56, 56, 56, 100)
		: IM_COL32(240, 240, 242, 180);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, childBg);

	ImGui::Indent(8.0f);
}

void render::endChildStyle()
{
	ImGui::Unindent();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

void render::setCheckboxStyle()
{
	if (cfg::is_dark_theme()) {
		ImGui::PushStyleColor(ImGuiCol_FrameBg,
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.11f, 0.11f, 0.13f, 1.0f)));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.13f, 0.13f, 0.15f, 1.0f)));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.14f, 0.14f, 0.17f, 1.0f)));
	} else {
		ImGui::PushStyleColor(ImGuiCol_FrameBg,
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f, 0.85f, 0.88f, 1.0f)));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.80f, 0.80f, 0.84f, 1.0f)));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f, 0.75f, 0.80f, 1.0f)));
	}
}

void render::endCheckboxStyle()
{
	ImGui::PopStyleColor(3);
}

float render::BeginCenteredColumn(const char* id, float maxW)
{
	float avail = ImGui::GetContentRegionAvail().x;
	float w = (avail > maxW) ? maxW : avail;

	float x = ImGui::GetCursorPosX();
	if (avail > w)
		ImGui::SetCursorPosX(x + (avail - w) * 0.5f);

	ImGui::BeginChild(id, ImVec2(w, 0), false);
	return w;
}

void render::EndCenteredColumn()
{
	ImGui::EndChild();
}

// ============================================================
// SOFT CONTAINER (cross-platform, pure ImGui draw calls)
// ============================================================

static struct SoftContainerState {
	ImVec2 contentMin;
	float padding;
	float rounding;
	bool active;
	int bgChannelIdx;
} g_softContainer = {};

static ImU32 GetSoftContainerShadowColor(int alpha)
{
	if (cfg::is_dark_theme())
		return IM_COL32(0, 0, 0, alpha);
	else
		return IM_COL32(0, 0, 0, (int)(alpha * 1.3f));
}

static ImU32 GetSoftContainerSurfaceColor()
{
	if (cfg::is_dark_theme()) {
		auto c = cfg::get_soft_container_dark();
		return IM_COL32((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), (int)(c.a*255));
	} else {
		auto c = cfg::get_soft_container_light();
		return IM_COL32((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), (int)(c.a*255));
	}
}

void render::BeginSoftContainer(float padding, float rounding)
{
	g_softContainer.padding = padding;
	g_softContainer.rounding = rounding;
	g_softContainer.active = true;

	g_softContainer.contentMin = ImGui::GetCursorScreenPos();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRectFullScreen();
	dl->ChannelsSplit(2);
	dl->ChannelsSetCurrent(1);

	ImGui::SetCursorScreenPos(ImVec2(
		g_softContainer.contentMin.x + padding,
		g_softContainer.contentMin.y + padding
	));

	ImGui::BeginGroup();
}

void render::EndSoftContainer()
{
	if (!g_softContainer.active) return;

	ImGui::EndGroup();

	ImVec2 contentMax = ImGui::GetItemRectMax();

	const float pad = g_softContainer.padding;
	const float rounding = g_softContainer.rounding;

	ImVec2 boxMin = g_softContainer.contentMin;
	ImVec2 boxMax = ImVec2(contentMax.x + pad, contentMax.y + pad);

	ImDrawList* dl = ImGui::GetWindowDrawList();

	dl->ChannelsSetCurrent(0);

	auto shadow_pass = [&](float grow, float offY, int a, float roundAdd)
		{
			float g = grow * 0.5f;
			dl->AddRectFilled(
				ImVec2(boxMin.x - g, boxMin.y + offY),
				ImVec2(boxMax.x + g, boxMax.y + g + offY),
				GetSoftContainerShadowColor(a),
				rounding + roundAdd
			);
		};

	shadow_pass(16.0f, 5.0f, 10, 10.0f);
	shadow_pass(10.0f, 3.0f, 18, 6.0f);
	shadow_pass(6.0f, 2.0f, 26, 2.0f);

	const ImU32 surfaceCol = GetSoftContainerSurfaceColor();
	dl->AddRectFilled(boxMin, boxMax, surfaceCol, rounding);

	dl->ChannelsMerge();
	dl->ChannelsSetCurrent(0);
	dl->PopClipRect();

	ImGui::SetCursorScreenPos(boxMin);
	const float advance_h = (boxMax.y - boxMin.y) + 8.0f;
	ImGui::Dummy(ImVec2(0.0f, advance_h));

	g_softContainer.active = false;
}

// ============================================================
// FONT SETUP (macOS — uses system SF fonts)
// ============================================================

void render::GUIInit()
{
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;

	// Font sizes
	const float sizeSmall = 13.0f;
	const float sizeRegular = 15.0f;
	const float sizeLarge = 17.0f;
	const float sizeBold = 20.0f;

	// macOS system font paths
	// SF Pro is available on macOS 11+, fallback to Helvetica Neue
	std::string regularFontPath;
	std::string boldFontPath;

	// Try SF Pro first (macOS 11+)
	const char* sfProPaths[] = {
		"/System/Library/Fonts/SFNS.ttf",
		"/Library/Fonts/SF-Pro-Text-Regular.otf",
		"/System/Library/Fonts/SFNSText.ttf",
		"/System/Library/Fonts/Helvetica.ttc",
	};
	const char* sfProBoldPaths[] = {
		"/System/Library/Fonts/SFNS.ttf",
		"/Library/Fonts/SF-Pro-Text-Bold.otf",
		"/System/Library/Fonts/SFNSText-Bold.otf",
		"/System/Library/Fonts/HelveticaNeue.ttc",
	};

	for (auto p : sfProPaths) {
		FILE* f = fopen(p, "rb");
		if (f) { fclose(f); regularFontPath = p; break; }
	}
	for (auto p : sfProBoldPaths) {
		FILE* f = fopen(p, "rb");
		if (f) { fclose(f); boldFontPath = p; break; }
	}

	// Icon config (reused with MergeMode)
	static const ImWchar mdi_ranges[] = { ICON_MIN_MDI, ICON_MAX_MDI, 0 };
	ImFontConfig icon_cfg;
	icon_cfg.MergeMode = true;
	icon_cfg.PixelSnapH = true;
	icon_cfg.OversampleH = 3;
	icon_cfg.OversampleV = 3;

	// Helper lambda to load font + merge icons
	auto loadFontWithIcons = [&](const std::string& path, float size) -> ImFont* {
		ImFont* font = nullptr;
		if (!path.empty())
			font = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
		if (!font)
			font = io.Fonts->AddFontDefault();
		// Merge MDI icons (static data — must not be freed by ImGui)
		icon_cfg.GlyphMinAdvanceX = size;
		icon_cfg.FontDataOwnedByAtlas = false;
		icon_cfg.GlyphOffset.y = 1.0f;
		io.Fonts->AddFontFromMemoryTTF((void*)mdi_font_data, mdi_font_size, size + 3.0f, &icon_cfg, mdi_ranges);
		icon_cfg.GlyphOffset.y = 0.0f;
		icon_cfg.FontDataOwnedByAtlas = true;
		return font;
	};

	// Load all font sizes with icons merged
	FontSmall = loadFontWithIcons(regularFontPath, sizeSmall);
	FontRegular = loadFontWithIcons(regularFontPath, sizeRegular);
	FontLarge = loadFontWithIcons(regularFontPath, sizeLarge);

	// Bold font
	FontBold = loadFontWithIcons(boldFontPath, sizeBold);

	// Keep Font15 as alias for backwards compatibility
	Font15 = FontRegular;

	// Style settings (theme-independent)
	ImGuiStyle& style = ImGui::GetStyle();
	style.Colors[ImGuiCol_NavCursor].w = 0.0f;
	style.ScrollbarSize = 4.0f;
	style.WindowPadding = ImVec2(5, 5);
	style.CellPadding = ImVec2(3, 8);
	style.FrameBorderSize = 1.0f;

	// Apply theme from config
	ApplyTheme(cfg::is_dark_theme());
}

void render::GUIShutDown()
{
	// Nothing to clean up on macOS — Metal/ImGui cleanup is in ViewController
}

// ============================================================
// THEME FUNCTIONS (cross-platform ImGui style, no DWM calls)
// ============================================================

void render::ApplyDarkTheme()
{
	ImGuiStyle& style = ImGui::GetStyle();

	// Window
	style.Colors[ImGuiCol_WindowBg] = ImColor(28, 27, 30, 255);
	style.Colors[ImGuiCol_ChildBg] = ImColor(30, 30, 30, 0);
	style.Colors[ImGuiCol_PopupBg] = ImColor(36, 35, 39, 255);
	style.Colors[ImGuiCol_FrameBg] = ImColor(0, 0, 0, 0);
	style.Colors[ImGuiCol_Border] = ImColor(40, 40, 40, 255);

	// Scrollbar
	style.Colors[ImGuiCol_ScrollbarBg] = ImColor(40, 40, 40, 0);
	style.Colors[ImGuiCol_ScrollbarGrab] = ImColor(60, 60, 60, 255);
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(80, 80, 80, 255);
	style.Colors[ImGuiCol_ScrollbarGrabActive] = ImColor(100, 100, 100, 255);

	// Buttons
	style.Colors[ImGuiCol_Button] = ImColor(42, 42, 42, 102);
	style.Colors[ImGuiCol_ButtonHovered] = ImColor(53, 54, 61, 150);
	style.Colors[ImGuiCol_ButtonActive] = ImColor(53, 54, 61, 100);

	// Headers
	style.Colors[ImGuiCol_Header] = ImColor(255, 255, 255, 0);
	style.Colors[ImGuiCol_HeaderHovered] = ImColor(53, 54, 61, 150);
	style.Colors[ImGuiCol_HeaderActive] = ImColor(53, 54, 61, 100);

	// Text
	style.Colors[ImGuiCol_Text] = ImColor(255, 255, 255, 255);
	style.Colors[ImGuiCol_TextDisabled] = ImColor(128, 128, 128, 255);
	style.Colors[ImGuiCol_InputTextCursor] = ImColor(255, 255, 255, 255);

	// Table
	style.Colors[ImGuiCol_TableHeaderBg] = ImColor(36, 35, 39, 255);
	style.Colors[ImGuiCol_TableBorderStrong] = ImColor(50, 50, 55, 255);
	style.Colors[ImGuiCol_TableBorderLight] = ImColor(40, 40, 45, 255);
	style.Colors[ImGuiCol_TableRowBg] = ImColor(0, 0, 0, 0);
	style.Colors[ImGuiCol_TableRowBgAlt] = ImColor(255, 255, 255, 10);
}

void render::ApplyLightTheme()
{
	ImGuiStyle& style = ImGui::GetStyle();

	// Window
	style.Colors[ImGuiCol_WindowBg] = ImColor(245, 245, 247, 255);
	style.Colors[ImGuiCol_ChildBg] = ImColor(255, 255, 255, 0);
	style.Colors[ImGuiCol_PopupBg] = ImColor(255, 255, 255, 255);
	style.Colors[ImGuiCol_FrameBg] = ImColor(0, 0, 0, 0);
	style.Colors[ImGuiCol_Border] = ImColor(200, 200, 200, 255);

	// Scrollbar
	style.Colors[ImGuiCol_ScrollbarBg] = ImColor(245, 245, 245, 0);
	style.Colors[ImGuiCol_ScrollbarGrab] = ImColor(180, 180, 180, 255);
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(150, 150, 150, 255);
	style.Colors[ImGuiCol_ScrollbarGrabActive] = ImColor(120, 120, 120, 255);

	// Buttons
	style.Colors[ImGuiCol_Button] = ImColor(220, 220, 220, 255);
	style.Colors[ImGuiCol_ButtonHovered] = ImColor(200, 200, 200, 255);
	style.Colors[ImGuiCol_ButtonActive] = ImColor(180, 180, 180, 255);

	// Headers
	style.Colors[ImGuiCol_Header] = ImColor(0, 0, 0, 0);
	style.Colors[ImGuiCol_HeaderHovered] = ImColor(220, 220, 225, 200);
	style.Colors[ImGuiCol_HeaderActive] = ImColor(200, 200, 205, 200);

	// Text
	style.Colors[ImGuiCol_Text] = ImColor(30, 30, 30, 255);
	style.Colors[ImGuiCol_TextDisabled] = ImColor(140, 140, 140, 255);
	style.Colors[ImGuiCol_InputTextCursor] = ImColor(30, 30, 30, 255);

	// Table
	style.Colors[ImGuiCol_TableHeaderBg] = ImColor(235, 235, 240, 255);
	style.Colors[ImGuiCol_TableBorderStrong] = ImColor(200, 200, 205, 255);
	style.Colors[ImGuiCol_TableBorderLight] = ImColor(215, 215, 220, 255);
	style.Colors[ImGuiCol_TableRowBg] = ImColor(0, 0, 0, 0);
	style.Colors[ImGuiCol_TableRowBgAlt] = ImColor(0, 0, 0, 10);
}

void render::ApplyTheme(bool dark)
{
	if (dark)
		ApplyDarkTheme();
	else
		ApplyLightTheme();
}

// ============================================================
// macOS WINDOW HELPERS
// ============================================================

void render::SetAlwaysOnTop(bool enabled)
{
    NSWindow* window = [NSApp keyWindow];
    if (!window)
        window = [[NSApp windows] firstObject];
    if (window) {
        [window setLevel:enabled ? NSFloatingWindowLevel : NSNormalWindowLevel];
    }
}

// ============================================================
// macOS MENU BAR STATUS ITEM (system tray equivalent)
// ============================================================

static NSStatusItem* g_statusItem = nil;
static bool g_tray_enabled = false;
static bool g_tray_lock_requested = false;
static bool g_tray_quit_requested = false;

@interface StatusItemHandler : NSObject
- (void)showApp:(id)sender;
- (void)lockVault:(id)sender;
- (void)quitApp:(id)sender;
@end

@implementation StatusItemHandler
- (void)showApp:(id)sender {
    render::ShowWindow();
}
- (void)lockVault:(id)sender {
    g_tray_lock_requested = true;
    render::ShowWindow();
}
- (void)quitApp:(id)sender {
    g_tray_quit_requested = true;
}
@end

static StatusItemHandler* g_statusHandler = nil;

static void EnsureStatusItem()
{
    if (g_statusItem) return;

    if (!g_statusHandler)
        g_statusHandler = [[StatusItemHandler alloc] init];

    g_statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    g_statusItem.button.image = [NSImage imageWithSystemSymbolName:@"lock.shield"
                                          accessibilityDescription:@"PasswordDefense"];
    if (!g_statusItem.button.image) {
        // Fallback if SF Symbol not available
        g_statusItem.button.title = @"PD";
    }

    NSMenu* menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"Show PasswordDefense" action:@selector(showApp:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Lock Vault" action:@selector(lockVault:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Quit" action:@selector(quitApp:) keyEquivalent:@"q"];

    for (NSMenuItem* item in menu.itemArray) {
        if (item.action) item.target = g_statusHandler;
    }

    g_statusItem.menu = menu;
}

static void RemoveStatusItem()
{
    if (g_statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
        g_statusItem = nil;
    }
}

void render::SetMinimizeToTray(bool enabled)
{
    g_tray_enabled = enabled;
    if (enabled)
        EnsureStatusItem();
    else
        RemoveStatusItem();
}

bool render::ConsumeTrayLock()
{
    bool val = g_tray_lock_requested;
    g_tray_lock_requested = false;
    return val;
}

bool render::ConsumeTrayQuit()
{
    bool val = g_tray_quit_requested;
    g_tray_quit_requested = false;
    return val;
}

void render::RemoveTrayIcon()
{
    RemoveStatusItem();
}

void render::ShowWindow()
{
    NSWindow* window = [[NSApp windows] firstObject];
    if (window) {
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void render::HideWindow()
{
    NSWindow* window = [NSApp keyWindow];
    if (!window)
        window = [[NSApp windows] firstObject];
    if (window) {
        [window orderOut:nil];
    }
}

bool render::IsWindowVisible()
{
    NSWindow* window = [[NSApp windows] firstObject];
    return window && [window isVisible];
}

void render::RequestAppQuit()
{
    g_tray_enabled = false; // Prevent windowShouldClose from hiding instead of closing
    [NSApp terminate:nil];
}

bool render::IsMinimizeToTrayEnabled()
{
    return g_tray_enabled;
}

bool render_app_is_active()
{
    return [NSApp isActive];
}

// DisplayProgram() is now defined in application.cpp
