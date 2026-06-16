#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <algorithm> // For std::clamp
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "plugin-support.h"
#include "trail-manager.hpp"

static constexpr const char *SETTING_MODE = "trail_mode";
static constexpr const char *SETTING_COLOR = "trail_color";
static constexpr const char *SETTING_CANVAS_MODE = "canvas_mode";
static constexpr const char *SETTING_SELECTED_MONITOR = "selected_monitor";
static constexpr const char *SETTING_MOUSEPAD_DPI = "mousepad_dpi";
static constexpr const char *SETTING_MOUSEPAD_WIDTH = "mousepad_width";
static constexpr const char *SETTING_MOUSEPAD_HEIGHT = "mousepad_height";
static constexpr const char *SETTING_BOUNDARY_MODE = "boundary_mode";
static constexpr const char *SETTING_SHOW_DPI_OVERLAY = "show_dpi_overlay";

static constexpr const char *MODE_SMOKE = "smoke";
static constexpr const char *MODE_PRECISION = "precision";
static constexpr const char *CANVAS_MODE_ALL_MONITORS = "all_monitors";
static constexpr const char *CANVAS_MODE_SPECIFIC_MONITOR = "specific_monitor";
static constexpr const char *CANVAS_MODE_MOUSEPAD = "mousepad";
static constexpr const char *BOUNDARY_MODE_DELIMITED = "delimited";
static constexpr const char *BOUNDARY_MODE_INFINITE_WRAP = "infinite_wrap";
static constexpr const char *BOUNDARY_MODE_INFINITE_RECENTER = "infinite_recenter";

/**
 * @brief Monitor information structure for canvas selection.
 */
struct MonitorInfo {
	std::string id;         // Unique identifier (platform-specific)
	std::string label;      // Display name "Monitor Name (WxH @ X,Y)"
	int x, y, width, height; // Monitor position and dimensions
	bool isPrimary;         // Whether this is the primary monitor
};

/**
 * @brief Canvas bounds representation.
 */
struct CanvasBounds {
	float width;
	float height;
	float offsetX = 0.0f; // For future multi-monitor absolute positioning
	float offsetY = 0.0f;
};

/**
 * @brief Platform-specific helper: Enumerate available monitors.
 * @param outMonitors Output vector of MonitorInfo structures.
 */
static void enumerate_monitors(std::vector<MonitorInfo>& outMonitors);

/**
 * @brief Map cursor position to canvas based on selected canvas mode.
 * @param cursorX, cursorY Cursor position in screen coordinates.
 * @param canvasMode Selected canvas mode.
 * @param selectedMonitorId Monitor ID if specific monitor mode.
 * @param monitors List of available monitors.
 * @param outCanvasBounds Output canvas bounds.
 * @return Normalized cursor position (0-1 range) to be scaled to OBS canvas.
 */
static std::pair<float, float> map_cursor_to_canvas(
	float cursorX, float cursorY,
	const char *canvasMode,
	const char *selectedMonitorId,
	const std::vector<MonitorInfo>& monitors,
	CanvasBounds& outCanvasBounds);

/**
 * @brief Apply boundary mode transformation before scaling to OBS canvas.
 * @param normX, normY Normalized coordinates (0-1).
 * @param boundaryMode Boundary behavior mode.
 * @return Transformed normalized coordinates.
 */
static std::pair<float, float> apply_boundary_mode(
	float normX, float normY,
	const char *boundaryMode);

/**
 * @brief Context state for the custom OBS Mouse Trail source.
 */
class MouseTrailSource {
public:
	obs_source_t *source;
	TrailManager trailManager;

	// Configuration settings
	float gravityFactor = 150.0f;             // Speed of anti-gravity upward drift (pixels/sec^2)
	uint64_t trailDurationNs = 1000000000ULL; // Lifetime of points in nanoseconds (1 second)
	bool smokeMode = true;

	// Trail appearance properties
	float colorR = 0.0f;
	float colorG = 0.8f;
	float colorB = 1.0f;

	// Canvas mapping configuration
	std::string canvasMode = CANVAS_MODE_ALL_MONITORS;
	std::string selectedMonitorId;
	std::vector<MonitorInfo> availableMonitors;
	float mousepadDpi = 800.0f;
	float mousepadWidth = 250.0f;  // mm
	float mousepadHeight = 250.0f; // mm
	std::string boundaryMode = BOUNDARY_MODE_DELIMITED;
	bool showDpiOverlay = false;

	// Cached last cursor position for mousepad mode
	float lastCursorX = 0.0f;
	float lastCursorY = 0.0f;
	bool hasLastCursor = false;

	// Cached canvas bounds
	CanvasBounds currentCanvasBounds{1920.0f, 1080.0f, 0.0f, 0.0f};

	MouseTrailSource(obs_source_t *source) : source(source) {}
};

static const char *mouse_trail_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("mouse_trail");
}

static void mouse_trail_apply_settings(MouseTrailSource *src, obs_data_t *settings)
{
	if (!src || !settings)
		return;

	bool settingsChanged = false;

	const char *mode = obs_data_get_string(settings, SETTING_MODE);
	src->smokeMode = (mode && strcmp(mode, MODE_PRECISION) != 0);

	const uint32_t color = static_cast<uint32_t>(obs_data_get_int(settings, SETTING_COLOR));
	// OBS color property stores packed color as BBGGRR.
	src->colorR = static_cast<float>(color & 0xFF) / 255.0f;
	src->colorG = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
	src->colorB = static_cast<float>((color >> 16) & 0xFF) / 255.0f;

	// Canvas mode
	const char *newCanvasMode = obs_data_get_string(settings, SETTING_CANVAS_MODE);
	if (newCanvasMode && src->canvasMode != newCanvasMode) {
		src->canvasMode = newCanvasMode;
		settingsChanged = true;
	}

	// Selected monitor
	const char *newSelectedMonitor = obs_data_get_string(settings, SETTING_SELECTED_MONITOR);
	if (newSelectedMonitor && src->selectedMonitorId != newSelectedMonitor) {
		src->selectedMonitorId = newSelectedMonitor;
		settingsChanged = true;
	}

	// Mousepad settings
	double dpi = obs_data_get_double(settings, SETTING_MOUSEPAD_DPI);
	if (dpi > 0) src->mousepadDpi = static_cast<float>(dpi);

	double padWidth = obs_data_get_double(settings, SETTING_MOUSEPAD_WIDTH);
	if (padWidth > 0) src->mousepadWidth = static_cast<float>(padWidth);

	double padHeight = obs_data_get_double(settings, SETTING_MOUSEPAD_HEIGHT);
	if (padHeight > 0) src->mousepadHeight = static_cast<float>(padHeight);

	src->showDpiOverlay = obs_data_get_bool(settings, SETTING_SHOW_DPI_OVERLAY);

	// Boundary mode
	const char *newBoundaryMode = obs_data_get_string(settings, SETTING_BOUNDARY_MODE);
	if (newBoundaryMode && src->boundaryMode != newBoundaryMode) {
		src->boundaryMode = newBoundaryMode;
		settingsChanged = true;
	}

	// Clear trail if canvas source or mode changed
	if (settingsChanged) {
		src->trailManager.clear();
		src->hasLastCursor = false;
		obs_log(LOG_INFO, "Canvas settings changed, trail cleared");
	}
}

static void *mouse_trail_create(obs_data_t *settings, obs_source_t *source)
{
	auto *src = new MouseTrailSource(source);
	enumerate_monitors(src->availableMonitors);
	obs_log(LOG_INFO, "mouse trail source created with %zu monitors", src->availableMonitors.size());
	mouse_trail_apply_settings(src, settings);
	return src;
}

static void mouse_trail_update(void *data, obs_data_t *settings)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	enumerate_monitors(src->availableMonitors);
	mouse_trail_apply_settings(src, settings);
}

static obs_properties_t *mouse_trail_get_properties(void *data)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *modeProp = obs_properties_add_list(
		props,
		SETTING_MODE,
		obs_module_text("trail.mode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	if (modeProp) {
		obs_property_list_add_string(modeProp, obs_module_text("trail.mode.smoke"), MODE_SMOKE);
		obs_property_list_add_string(modeProp, obs_module_text("trail.mode.precision"), MODE_PRECISION);
	}

	obs_properties_add_color(props, SETTING_COLOR, obs_module_text("trail.color"));

	// Canvas mode selection
	obs_property_t *canvasModeProp = obs_properties_add_list(
		props,
		SETTING_CANVAS_MODE,
		obs_module_text("canvas.mode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	if (canvasModeProp) {
		obs_property_list_add_string(canvasModeProp, obs_module_text("canvas.mode.all_monitors"), 
			CANVAS_MODE_ALL_MONITORS);
		obs_property_list_add_string(canvasModeProp, obs_module_text("canvas.mode.specific_monitor"), 
			CANVAS_MODE_SPECIFIC_MONITOR);
		obs_property_list_add_string(canvasModeProp, obs_module_text("canvas.mode.mousepad"), 
			CANVAS_MODE_MOUSEPAD);
	}

	// Monitor selection dropdown
	obs_property_t *monitorProp = obs_properties_add_list(
		props,
		SETTING_SELECTED_MONITOR,
		obs_module_text("canvas.monitor"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	if (monitorProp && src) {
		for (const auto& monitor : src->availableMonitors) {
			obs_property_list_add_string(monitorProp, monitor.label.c_str(), monitor.id.c_str());
		}
	}

	// Mousepad DPI setting
	obs_properties_add_float(props, SETTING_MOUSEPAD_DPI, obs_module_text("mousepad.dpi"),
		100.0, 3200.0, 1.0);

	// Mousepad width (mm)
	obs_properties_add_float(props, SETTING_MOUSEPAD_WIDTH, obs_module_text("mousepad.width"),
		50.0, 1000.0, 1.0);

	// Mousepad height (mm)
	obs_properties_add_float(props, SETTING_MOUSEPAD_HEIGHT, obs_module_text("mousepad.height"),
		50.0, 1000.0, 1.0);

	obs_properties_add_bool(props, SETTING_SHOW_DPI_OVERLAY, obs_module_text("mousepad.show_dpi_overlay"));

	// Boundary mode selection
	obs_property_t *boundaryModeProp = obs_properties_add_list(
		props,
		SETTING_BOUNDARY_MODE,
		obs_module_text("boundary.mode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	if (boundaryModeProp) {
		obs_property_list_add_string(boundaryModeProp, obs_module_text("boundary.mode.delimited"), 
			BOUNDARY_MODE_DELIMITED);
		obs_property_list_add_string(boundaryModeProp, obs_module_text("boundary.mode.infinite_wrap"), 
			BOUNDARY_MODE_INFINITE_WRAP);
		obs_property_list_add_string(boundaryModeProp, obs_module_text("boundary.mode.infinite_recenter"), 
			BOUNDARY_MODE_INFINITE_RECENTER);
	}

	return props;
}

static void mouse_trail_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_MODE, MODE_SMOKE);
	obs_data_set_default_int(settings, SETTING_COLOR, 0x00CCFF);
	obs_data_set_default_string(settings, SETTING_CANVAS_MODE, CANVAS_MODE_ALL_MONITORS);
	obs_data_set_default_double(settings, SETTING_MOUSEPAD_DPI, 800.0);
	obs_data_set_default_double(settings, SETTING_MOUSEPAD_WIDTH, 250.0);
	obs_data_set_default_double(settings, SETTING_MOUSEPAD_HEIGHT, 250.0);
	obs_data_set_default_string(settings, SETTING_BOUNDARY_MODE, BOUNDARY_MODE_DELIMITED);
	obs_data_set_default_bool(settings, SETTING_SHOW_DPI_OVERLAY, false);
}

// ============================================================================
// PLATFORM-SPECIFIC IMPLEMENTATIONS
// ============================================================================

#ifdef _WIN32

/**
 * @brief Windows: Enumerate all monitors.
 */
static void enumerate_monitors(std::vector<MonitorInfo>& outMonitors)
{
	outMonitors.clear();

	// Get virtual screen bounds
	int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	// Add "All Monitors" option first
	MonitorInfo allMonitors;
	allMonitors.id = "all_monitors_combined";
	allMonitors.label = "All Monitors (" + std::to_string(virtualWidth) + "x" + std::to_string(virtualHeight) + ")";
	allMonitors.x = virtualLeft;
	allMonitors.y = virtualTop;
	allMonitors.width = virtualWidth;
	allMonitors.height = virtualHeight;
	allMonitors.isPrimary = false;
	outMonitors.push_back(allMonitors);

	// Enumerate individual monitors
	MONITORINFOEX miex;
	ZeroMemory(&miex, sizeof(miex));
	miex.cbSize = sizeof(miex);

	auto enumProc = [](HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
		UNUSED_PARAMETER(hdcMonitor);
		auto *monitors = reinterpret_cast<std::vector<MonitorInfo>*>(dwData);

		MONITORINFOEX miex;
		ZeroMemory(&miex, sizeof(miex));
		miex.cbSize = sizeof(miex);
		if (!GetMonitorInfoA(hMonitor, (MONITORINFO*)&miex))
			return TRUE;

		MonitorInfo info;
		info.id = miex.szDevice;
		int width = lprcMonitor->right - lprcMonitor->left;
		int height = lprcMonitor->bottom - lprcMonitor->top;
		info.label = std::string(miex.szDevice) + " (" + std::to_string(width) + "x" + std::to_string(height) + 
					 " @ " + std::to_string(lprcMonitor->left) + "," + std::to_string(lprcMonitor->top) + ")";
		info.x = lprcMonitor->left;
		info.y = lprcMonitor->top;
		info.width = width;
		info.height = height;
		info.isPrimary = (miex.dwFlags & MONITORINFOF_PRIMARY) != 0;

		monitors->push_back(info);
		return TRUE;
	};

	EnumDisplayMonitors(NULL, NULL, enumProc, reinterpret_cast<LPARAM>(&outMonitors));

	obs_log(LOG_INFO, "Enumerated %zu monitors", outMonitors.size());
}

#else

/**
 * @brief Fallback: Enumerate monitors on macOS/Linux.
 * For now, returns a single virtual monitor.
 */
static void enumerate_monitors(std::vector<MonitorInfo>& outMonitors)
{
	outMonitors.clear();
	
	MonitorInfo fallback;
	fallback.id = "fallback_monitor";
	fallback.label = "Primary Monitor (1920x1080)";
	fallback.x = 0;
	fallback.y = 0;
	fallback.width = 1920;
	fallback.height = 1080;
	fallback.isPrimary = true;
	outMonitors.push_back(fallback);

	obs_log(LOG_WARNING, "Using fallback monitor enumeration");
}

#endif

// ============================================================================
// CANVAS MAPPING HELPERS
// ============================================================================

/**
 * @brief Map cursor to canvas based on mode.
 */
static std::pair<float, float> map_cursor_to_canvas(
	float cursorX, float cursorY,
	const char *canvasMode,
	const char *selectedMonitorId,
	const std::vector<MonitorInfo>& monitors,
	CanvasBounds& outCanvasBounds)
{
	if (!canvasMode) {
		return {0.0f, 0.0f};
	}

	// All monitors mode: map to virtual screen bounds
	if (strcmp(canvasMode, CANVAS_MODE_ALL_MONITORS) == 0) {
		int virtualLeft = 0;
		int virtualTop = 0;
		int virtualWidth = 1920;
		int virtualHeight = 1080;

		#ifdef _WIN32
		virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
		virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
		virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		#endif

		outCanvasBounds.width = static_cast<float>(virtualWidth);
		outCanvasBounds.height = static_cast<float>(virtualHeight);
		outCanvasBounds.offsetX = static_cast<float>(virtualLeft);
		outCanvasBounds.offsetY = static_cast<float>(virtualTop);

		if (virtualWidth > 0 && virtualHeight > 0) {
			float normX = (cursorX - virtualLeft) / virtualWidth;
			float normY = (cursorY - virtualTop) / virtualHeight;
			return {normX, normY};
		}
		return {0.0f, 0.0f};
	}

	// Specific monitor mode: map to selected monitor bounds
	if (strcmp(canvasMode, CANVAS_MODE_SPECIFIC_MONITOR) == 0 && selectedMonitorId) {
		for (const auto& monitor : monitors) {
			if (monitor.id == selectedMonitorId) {
				outCanvasBounds.width = static_cast<float>(monitor.width);
				outCanvasBounds.height = static_cast<float>(monitor.height);
				outCanvasBounds.offsetX = static_cast<float>(monitor.x);
				outCanvasBounds.offsetY = static_cast<float>(monitor.y);

				float normX = (cursorX - monitor.x) / static_cast<float>(monitor.width);
				float normY = (cursorY - monitor.y) / static_cast<float>(monitor.height);
				return {normX, normY};
			}
		}
	}

	// Mousepad mode: Not handled here, returns zero delta
	// (handled separately in tick with delta tracking)
	return {0.0f, 0.0f};
}

/**
 * @brief Apply boundary transformation.
 */
static std::pair<float, float> apply_boundary_mode(
	float normX, float normY,
	const char *boundaryMode)
{
	if (!boundaryMode)
		return {normX, normY};

	// Delimited: clamp to 0-1
	if (strcmp(boundaryMode, BOUNDARY_MODE_DELIMITED) == 0) {
		return {
			std::clamp(normX, 0.0f, 1.0f),
			std::clamp(normY, 0.0f, 1.0f)
		};
	}

	// Infinite Wrap: wrap around
	if (strcmp(boundaryMode, BOUNDARY_MODE_INFINITE_WRAP) == 0) {
		float x = fmod(normX, 1.0f);
		float y = fmod(normY, 1.0f);
		if (x < 0.0f) x += 1.0f;
		if (y < 0.0f) y += 1.0f;
		return {x, y};
	}

	// Infinite Recenter: jump to opposite side and continue
	if (strcmp(boundaryMode, BOUNDARY_MODE_INFINITE_RECENTER) == 0) {
		float x = normX;
		float y = normY;
		
		if (x < 0.0f) x = 1.0f + fmod(x, 1.0f);
		else if (x > 1.0f) x = fmod(x, 1.0f);

		if (y < 0.0f) y = 1.0f + fmod(y, 1.0f);
		else if (y > 1.0f) y = fmod(y, 1.0f);

		return {x, y};
	}

	return {normX, normY};
}

static void mouse_trail_destroy(void *data)
{
	delete static_cast<MouseTrailSource*>(data);
}

static void draw_filled_rect(float x, float y, float w, float h)
{
	gs_render_start(true);
	gs_vertex2f(x, y);
	gs_vertex2f(x + w, y);
	gs_vertex2f(x, y + h);
	gs_vertex2f(x + w, y + h);
	gs_render_stop(GS_TRISTRIP);
}

static uint8_t glyph_row_bits(char c, int row)
{
	static const uint8_t GLYPH_SPACE[7] = {0, 0, 0, 0, 0, 0, 0};
	static const uint8_t GLYPH_COLON[7] = {0, 0b00100, 0, 0, 0b00100, 0, 0};
	static const uint8_t GLYPH_D[7] = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
	static const uint8_t GLYPH_P[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000};
	static const uint8_t GLYPH_I[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111};
	static const uint8_t GLYPH_0[7] = {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110};
	static const uint8_t GLYPH_1[7] = {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
	static const uint8_t GLYPH_2[7] = {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111};
	static const uint8_t GLYPH_3[7] = {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110};
	static const uint8_t GLYPH_4[7] = {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010};
	static const uint8_t GLYPH_5[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110};
	static const uint8_t GLYPH_6[7] = {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110};
	static const uint8_t GLYPH_7[7] = {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000};
	static const uint8_t GLYPH_8[7] = {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110};
	static const uint8_t GLYPH_9[7] = {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110};

	const uint8_t *glyph = GLYPH_SPACE;
	const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;

	switch (upper) {
	case ':': glyph = GLYPH_COLON; break;
	case 'D': glyph = GLYPH_D; break;
	case 'P': glyph = GLYPH_P; break;
	case 'I': glyph = GLYPH_I; break;
	case '0': glyph = GLYPH_0; break;
	case '1': glyph = GLYPH_1; break;
	case '2': glyph = GLYPH_2; break;
	case '3': glyph = GLYPH_3; break;
	case '4': glyph = GLYPH_4; break;
	case '5': glyph = GLYPH_5; break;
	case '6': glyph = GLYPH_6; break;
	case '7': glyph = GLYPH_7; break;
	case '8': glyph = GLYPH_8; break;
	case '9': glyph = GLYPH_9; break;
	default: break;
	}

	return (row >= 0 && row < 7) ? glyph[row] : 0;
}

static void draw_bitmap_text_centered(const char *text, float canvasW, float canvasH, float pixelSize, float bottomMargin)
{
	if (!text || !*text)
		return;

	const float glyphW = 5.0f * pixelSize;
	const float glyphH = 7.0f * pixelSize;
	const float spacing = pixelSize * 1.5f;
	const size_t len = strlen(text);
	if (len == 0)
		return;

	const float textW = len * glyphW + (len > 1 ? (len - 1) * spacing : 0.0f);
	float x = (canvasW - textW) * 0.5f;
	const float y = std::max(0.0f, canvasH - bottomMargin - glyphH);

	for (size_t i = 0; i < len; ++i) {
		for (int row = 0; row < 7; ++row) {
			const uint8_t bits = glyph_row_bits(text[i], row);
			for (int col = 0; col < 5; ++col) {
				if ((bits & (1u << (4 - col))) != 0) {
					draw_filled_rect(
						x + static_cast<float>(col) * pixelSize,
						y + static_cast<float>(row) * pixelSize,
						pixelSize,
						pixelSize);
				}
			}
		}
		x += glyphW + spacing;
	}
}

static uint32_t mouse_trail_get_width(void *data)
{
	UNUSED_PARAMETER(data);

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi))
		return ovi.base_width;

	return 1920;
}

static uint32_t mouse_trail_get_height(void *data)
{
	UNUSED_PARAMETER(data);

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi))
		return ovi.base_height;

	return 1080;
}

/**
 * @brief Updates logic on the OBS tick thread.
 */
static void mouse_trail_tick(void *data, float seconds)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	if (!src) return;

	float mouseX = 0.0f;
	float mouseY = 0.0f;
	bool haveMappedCursor = false;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

#ifdef _WIN32
	POINT mousePos;
	if (GetCursorPos(&mousePos)) {
		// Use new mapping pipeline
		if (strcmp(src->canvasMode.c_str(), CANVAS_MODE_MOUSEPAD) == 0) {
			// Mousepad mode: calculate delta movement
			float deltaX = mousePos.x - src->lastCursorX;
			float deltaY = mousePos.y - src->lastCursorY;

			// Convert screen pixels to millimeters based on DPI
			// DPI = pixels per inch, 1 inch = 25.4 mm
			float mmPerPixel = 25.4f / src->mousepadDpi;
			float deltaMm_X = deltaX * mmPerPixel;
			float deltaMm_Y = deltaY * mmPerPixel;

			// Calculate normalized movement relative to mousepad size
			float normDelta_X = deltaMm_X / src->mousepadWidth;
			float normDelta_Y = deltaMm_Y / src->mousepadHeight;

			// Update cursor position with wrapping
			float normX = fmod(src->lastCursorX / src->currentCanvasBounds.width + normDelta_X, 1.0f);
			float normY = fmod(src->lastCursorY / src->currentCanvasBounds.height + normDelta_Y, 1.0f);
			if (normX < 0.0f) normX += 1.0f;
			if (normY < 0.0f) normY += 1.0f;

			// Apply boundary mode
			auto [boundaryX, boundaryY] = apply_boundary_mode(normX, normY, src->boundaryMode.c_str());
			mouseX = boundaryX * ovi.base_width;
			mouseY = boundaryY * ovi.base_height;

			src->lastCursorX = mousePos.x;
			src->lastCursorY = mousePos.y;
			haveMappedCursor = true;
		} else {
			// Monitor-based modes
			auto [normX, normY] = map_cursor_to_canvas(
				static_cast<float>(mousePos.x),
				static_cast<float>(mousePos.y),
				src->canvasMode.c_str(),
				src->selectedMonitorId.c_str(),
				src->availableMonitors,
				src->currentCanvasBounds
			);

			// Apply boundary mode
			auto [boundaryX, boundaryY] = apply_boundary_mode(normX, normY, src->boundaryMode.c_str());

			// Scale to OBS canvas
			mouseX = boundaryX * ovi.base_width;
			mouseY = boundaryY * ovi.base_height;
			haveMappedCursor = true;
		}
	}
#else
	// macOS/Linux: fallback to simple mapping
	// TODO: Implement platform-specific cursor and monitor enumeration
	haveMappedCursor = false;
#endif

	// 1. Add point to queue
	if (haveMappedCursor) {
		src->trailManager.addPoint(mouseX, mouseY);
	}

	// 2. Clean up aged points
	src->trailManager.cleanup(src->trailDurationNs);

	// 3. Move points upward simulating anti-gravity (frame-rate independent)
	if (src->smokeMode)
		src->trailManager.applyGravity(src->gravityFactor, seconds);

	static uint64_t lastTickLogNs = 0;
	uint64_t now = os_gettime_ns();
	if (now - lastTickLogNs > 2000000000ULL) {
		lastTickLogNs = now;
		obs_log(LOG_INFO, "tick: mapped cursor x=%.1f y=%.1f mode=%s", mouseX, mouseY, src->canvasMode.c_str());
	}
}

/**
 * @brief Renders the mouse trail using the OBS Graphics system.
 */
static void mouse_trail_render(void *data, gs_effect_t *effect)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	if (!src) return;

	// Retrieve a copy of points under lock to prevent data races with the tick thread
	std::deque<Point> points = src->trailManager.getPoints();
	const bool canDrawTrail = points.size() >= 2;
	const bool showDpiOverlay = src->showDpiOverlay;
	if (!canDrawTrail && !showDpiOverlay)
		return;

	static uint64_t lastRenderLogNs = 0;
	uint64_t nowNs = os_gettime_ns();
	if (nowNs - lastRenderLogNs > 2000000000ULL) {
		lastRenderLogNs = nowNs;
			if (canDrawTrail) {
				obs_log(LOG_INFO, "render: points=%d first=(%.1f, %.1f) last=(%.1f, %.1f)", (int)points.size(),
					points.front().x, points.front().y, points.back().x, points.back().y);
			}
		}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!solid) return;

	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	if (!colorParam || !tech) return;

	uint64_t currentTime = os_gettime_ns();

	// Begin rendering techniques and passes
	size_t passes = gs_technique_begin(tech);
	for (size_t pass = 0; pass < passes; pass++) {
		gs_technique_begin_pass(tech, pass);

		if (canDrawTrail) {
			const float thickness = 6.0f;
			for (size_t i = 1; i < points.size(); i++) {
				const Point &a = points[i - 1];
				const Point &b = points[i];

				float dx = b.x - a.x;
				float dy = b.y - a.y;
				float len = std::sqrt(dx * dx + dy * dy);
				if (len < 0.001f)
					continue;

				float nx = -dy / len;
				float ny = dx / len;
				float hx = nx * (thickness * 0.5f);
				float hy = ny * (thickness * 0.5f);

				uint64_t age = (currentTime >= b.timestamp) ? (currentTime - b.timestamp) : 0;
				double normalized_age = static_cast<double>(age) / static_cast<double>(src->trailDurationNs);
				float alpha = 1.0f - static_cast<float>(normalized_age);
				alpha = std::clamp(alpha, 0.0f, 1.0f);

				struct vec4 segColor;
				vec4_set(&segColor, src->colorR, src->colorG, src->colorB, alpha);
				gs_effect_set_vec4(colorParam, &segColor);

				gs_render_start(true);
				gs_vertex2f(a.x - hx, a.y - hy);
				gs_vertex2f(a.x + hx, a.y + hy);
				gs_vertex2f(b.x - hx, b.y - hy);
				gs_vertex2f(b.x + hx, b.y + hy);
				gs_render_stop(GS_TRISTRIP);
			}
		}

		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);

	if (showDpiOverlay) {
		struct obs_video_info ovi;
		const float canvasW = obs_get_video_info(&ovi) ? static_cast<float>(ovi.base_width) : 1920.0f;
		const float canvasH = obs_get_video_info(&ovi) ? static_cast<float>(ovi.base_height) : 1080.0f;
		const float pixelSize = std::max(2.0f, std::round(canvasH / 360.0f));
		const float bottomMargin = std::max(8.0f, canvasH * 0.04f);

		char dpiText[64];
		snprintf(dpiText, sizeof(dpiText), "DPI: %.0f", src->mousepadDpi);

		size_t textPasses = gs_technique_begin(tech);
		for (size_t pass = 0; pass < textPasses; ++pass) {
			gs_technique_begin_pass(tech, pass);
			struct vec4 textColor;
			vec4_set(&textColor, 1.0f, 1.0f, 1.0f, 0.9f);
			gs_effect_set_vec4(colorParam, &textColor);
			draw_bitmap_text_centered(dpiText, canvasW, canvasH, pixelSize, bottomMargin);
			gs_technique_end_pass(tech);
		}
		gs_technique_end(tech);
	}

	UNUSED_PARAMETER(effect); // We use the standard OBS_EFFECT_DEFAULT instead of the passed parameter
}

// Register custom source structure
struct obs_source_info mouse_trail_info = {
	.id = "mouse_trail_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW, // CUSTOM_DRAW is required to execute video_render
	.get_name = mouse_trail_get_name,
	.create = mouse_trail_create,
	.destroy = mouse_trail_destroy,
	.get_width = mouse_trail_get_width,
	.get_height = mouse_trail_get_height,
	.get_defaults = mouse_trail_get_defaults,
	.get_properties = mouse_trail_get_properties,
	.update = mouse_trail_update,
	.video_tick = mouse_trail_tick,
	.video_render = mouse_trail_render,
};
