#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <graphics/image-file.h>
#include <algorithm> // For std::clamp
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

#include "plugin-support.h"
#include "trail-manager.hpp"

static constexpr const char *SETTING_MODE = "trail_mode";
static constexpr const char *SETTING_COLOR = "trail_color";
static constexpr const char *SETTING_PERSISTENCE_MODE = "trail_persistence_mode";
static constexpr const char *SETTING_TRAIL_DURATION_SECONDS = "trail_duration_seconds";
static constexpr const char *SETTING_TRAIL_THICKNESS = "trail_thickness";
static constexpr const char *SETTING_DOT_SIZE = "dot_size";
static constexpr const char *SETTING_DOT_IMAGE_PATH = "dot_image_path";
static constexpr const char *SETTING_DOT_IMAGE_CLEAR_FLAG = "dot_image_clear_flag";
static constexpr const char *SETTING_TAIL_END_OPACITY = "tail_end_opacity";
static constexpr const char *SETTING_TAIL_OPACITY_FALLOFF = "tail_opacity_falloff";
static constexpr const char *SETTING_CANVAS_MODE = "canvas_mode";
static constexpr const char *SETTING_SELECTED_MONITOR = "selected_monitor";
static constexpr const char *SETTING_MOUSEPAD_DPI = "mousepad_dpi";
static constexpr const char *SETTING_MOUSEPAD_DPI_MANUAL = "mousepad_dpi_manual";
static constexpr const char *SETTING_MOUSEPAD_DPI_EFFECTIVE = "mousepad_dpi_effective";
static constexpr const char *SETTING_MOUSEPAD_WIDTH = "mousepad_width";
static constexpr const char *SETTING_MOUSEPAD_HEIGHT = "mousepad_height";
static constexpr const char *SETTING_MOUSEPAD_RAW_INPUT_ACTIVE = "mousepad_raw_input_active";
static constexpr const char *SETTING_BOUNDARY_MODE = "boundary_mode";
static constexpr const char *SETTING_SHOW_DPI_OVERLAY = "show_dpi_overlay";

static constexpr const char *MODE_SMOKE = "smoke";
static constexpr const char *MODE_PRECISION = "precision";
static constexpr const char *PERSISTENCE_MODE_TIMED = "timed";
static constexpr const char *PERSISTENCE_MODE_FOREVER = "forever";
static constexpr const char *PERSISTENCE_MODE_LASER = "laser";
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

#ifdef _WIN32
namespace raw_input_subsystem {
static std::atomic<long> pendingDx{0};
static std::atomic<long> pendingDy{0};
static std::atomic<int> refCount{0};
static std::thread worker;
static std::mutex stateMutex;
static std::condition_variable readyCv;
static bool ready = false;
static bool initialized = false;
static HWND hwnd = nullptr;
static DWORD threadId = 0;

static LRESULT CALLBACK raw_input_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	UNUSED_PARAMETER(hWnd);
	UNUSED_PARAMETER(wParam);

	if (msg == WM_INPUT) {
		UINT dataSize = 0;
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &dataSize,
			sizeof(RAWINPUTHEADER)) != 0 || dataSize == 0) {
			return 0;
		}

		std::vector<uint8_t> buffer(dataSize);
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &dataSize,
			sizeof(RAWINPUTHEADER)) != dataSize) {
			return 0;
		}

		RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(buffer.data());
		if (raw->header.dwType == RIM_TYPEMOUSE) {
			const LONG dx = raw->data.mouse.lLastX;
			const LONG dy = raw->data.mouse.lLastY;
			if (dx != 0)
				pendingDx.fetch_add(dx, std::memory_order_relaxed);
			if (dy != 0)
				pendingDy.fetch_add(dy, std::memory_order_relaxed);
		}
		return 0;
	}

	if (msg == WM_CLOSE) {
		DestroyWindow(hWnd);
		return 0;
	}

	if (msg == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void raw_input_thread_main()
{
	threadId = GetCurrentThreadId();

	WNDCLASSEXA wc;
	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = raw_input_wndproc;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "MouseTrailRawInputWindow";

	ATOM classAtom = RegisterClassExA(&wc);
	if (!classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		std::lock_guard<std::mutex> lock(stateMutex);
		ready = true;
		initialized = false;
		readyCv.notify_all();
		return;
	}

	hwnd = CreateWindowExA(
		0,
		"MouseTrailRawInputWindow",
		"MouseTrailRawInputWindow",
		0,
		0,
		0,
		0,
		0,
		HWND_MESSAGE,
		nullptr,
		GetModuleHandleA(nullptr),
		nullptr);

	if (!hwnd) {
		std::lock_guard<std::mutex> lock(stateMutex);
		ready = true;
		initialized = false;
		readyCv.notify_all();
		return;
	}

	RAWINPUTDEVICE rid;
	rid.usUsagePage = 0x01;
	rid.usUsage = 0x02;
	rid.dwFlags = RIDEV_INPUTSINK;
	rid.hwndTarget = hwnd;

	if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
		DestroyWindow(hwnd);
		hwnd = nullptr;
		std::lock_guard<std::mutex> lock(stateMutex);
		ready = true;
		initialized = false;
		readyCv.notify_all();
		return;
	}

	{
		std::lock_guard<std::mutex> lock(stateMutex);
		ready = true;
		initialized = true;
		readyCv.notify_all();
	}

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	hwnd = nullptr;
	threadId = 0;
	pendingDx.store(0, std::memory_order_relaxed);
	pendingDy.store(0, std::memory_order_relaxed);
}

static bool init()
{
	int prev = refCount.fetch_add(1, std::memory_order_relaxed);
	if (prev > 0)
		return initialized;

	{
		std::lock_guard<std::mutex> lock(stateMutex);
		ready = false;
		initialized = false;
	}

	worker = std::thread(raw_input_thread_main);

	std::unique_lock<std::mutex> lock(stateMutex);
	readyCv.wait_for(lock, std::chrono::milliseconds(1500), [] { return ready; });
	return initialized;
}

static void shutdown()
{
	int prev = refCount.fetch_sub(1, std::memory_order_relaxed);
	if (prev <= 1) {
		if (threadId != 0)
			PostThreadMessage(threadId, WM_QUIT, 0, 0);
		if (worker.joinable())
			worker.join();
		initialized = false;
	}
}

static bool is_available()
{
	return initialized;
}

static bool consume_delta(float &dx, float &dy)
{
	const long rawDx = pendingDx.exchange(0, std::memory_order_relaxed);
	const long rawDy = pendingDy.exchange(0, std::memory_order_relaxed);
	dx = static_cast<float>(rawDx);
	dy = static_cast<float>(rawDy);
	return rawDx != 0 || rawDy != 0;
}
} // namespace raw_input_subsystem
#endif

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

static bool is_persistence_timed(const char *mode)
{
	return mode && strcmp(mode, PERSISTENCE_MODE_TIMED) == 0;
}

static const char *DOT_IMAGE_FILTER = "PNG Image (*.png);;All Files (*.*)";

static float get_system_mouse_dpi_estimate()
{
#ifdef _WIN32
	// Windows does not expose true hardware mouse DPI over a standard API.
	// Use pointer speed as a system-based estimate around a 800 DPI baseline.
	int pointerSpeed = 10;
	if (SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &pointerSpeed, 0)) {
		pointerSpeed = std::clamp(pointerSpeed, 1, 20);
		return 800.0f * (static_cast<float>(pointerSpeed) / 10.0f);
	}
	return 800.0f;
#else
	return 800.0f;
#endif
}

static float get_effective_mouse_dpi(bool manual, float manualDpi)
{
	if (manual)
		return std::max(1.0f, manualDpi);
	return std::max(1.0f, get_system_mouse_dpi_estimate());
}

static void sync_runtime_status_settings(obs_data_t *settings)
{
	if (!settings)
		return;

#ifdef _WIN32
	obs_data_set_bool(settings, SETTING_MOUSEPAD_RAW_INPUT_ACTIVE, raw_input_subsystem::is_available());
#else
	obs_data_set_bool(settings, SETTING_MOUSEPAD_RAW_INPUT_ACTIVE, false);
#endif
}

static void sync_effective_dpi_display(obs_data_t *settings)
{
	if (!settings)
		return;

	const bool manualMode = obs_data_get_bool(settings, SETTING_MOUSEPAD_DPI_MANUAL);
	const double manualDpi = obs_data_get_double(settings, SETTING_MOUSEPAD_DPI);
	const float effectiveDpi = get_effective_mouse_dpi(manualMode, static_cast<float>(manualDpi));

	char dpiStr[64];
	snprintf(dpiStr, sizeof(dpiStr), "%.0f DPI (%.0f counts/inch)", effectiveDpi, effectiveDpi);
	obs_data_set_string(settings, SETTING_MOUSEPAD_DPI_EFFECTIVE, dpiStr);
}

// Thread-local storage for source pointer to pass to button callback
static thread_local obs_source_t *g_button_callback_source = nullptr;

static bool monitor_exists_by_id(const std::vector<MonitorInfo> &monitors, const std::string &id)
{
	for (const auto &monitor : monitors) {
		if (monitor.id == id)
			return true;
	}
	return false;
}

static std::string get_fallback_monitor_id(const std::vector<MonitorInfo> &monitors)
{
	for (const auto &monitor : monitors) {
		if (monitor.isPrimary)
			return monitor.id;
	}
	if (!monitors.empty())
		return monitors.front().id;
	return std::string();
}

static bool dpi_settings_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	sync_effective_dpi_display(settings);
	return true;
}

static bool canvas_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *canvasMode = obs_data_get_string(settings, SETTING_CANVAS_MODE);
	const bool isSpecificMonitor = canvasMode && strcmp(canvasMode, CANVAS_MODE_SPECIFIC_MONITOR) == 0;
	const bool isMousepad = canvasMode && strcmp(canvasMode, CANVAS_MODE_MOUSEPAD) == 0;

	obs_property_t *monitorProp = obs_properties_get(props, SETTING_SELECTED_MONITOR);
	obs_property_t *dpiProp = obs_properties_get(props, SETTING_MOUSEPAD_DPI);
	obs_property_t *dpiManualProp = obs_properties_get(props, SETTING_MOUSEPAD_DPI_MANUAL);
	obs_property_t *dpiEffectiveProp = obs_properties_get(props, SETTING_MOUSEPAD_DPI_EFFECTIVE);
	obs_property_t *padWProp = obs_properties_get(props, SETTING_MOUSEPAD_WIDTH);
	obs_property_t *padHProp = obs_properties_get(props, SETTING_MOUSEPAD_HEIGHT);
	obs_property_t *rawStatusProp = obs_properties_get(props, SETTING_MOUSEPAD_RAW_INPUT_ACTIVE);
	const bool manualDpi = obs_data_get_bool(settings, SETTING_MOUSEPAD_DPI_MANUAL);

	if (monitorProp)
		obs_property_set_visible(monitorProp, isSpecificMonitor);
	if (dpiManualProp)
		obs_property_set_visible(dpiManualProp, isMousepad);
	if (dpiEffectiveProp)
		obs_property_set_visible(dpiEffectiveProp, isMousepad);
	if (dpiProp)
		obs_property_set_visible(dpiProp, isMousepad && manualDpi);
	if (padWProp)
		obs_property_set_visible(padWProp, isMousepad);
	if (padHProp)
		obs_property_set_visible(padHProp, isMousepad);
	if (rawStatusProp)
		obs_property_set_visible(rawStatusProp, isMousepad);

	if (isMousepad)
		sync_effective_dpi_display(settings);

	return true;
}

static bool persistence_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *persistenceMode = obs_data_get_string(settings, SETTING_PERSISTENCE_MODE);
	const bool isTimed = is_persistence_timed(persistenceMode);

	obs_property_t *durationProp = obs_properties_get(props, SETTING_TRAIL_DURATION_SECONDS);
	obs_property_t *tailEndOpacityProp = obs_properties_get(props, SETTING_TAIL_END_OPACITY);
	obs_property_t *tailOpacityFalloffProp = obs_properties_get(props, SETTING_TAIL_OPACITY_FALLOFF);

	if (durationProp)
		obs_property_set_visible(durationProp, isTimed);
	if (tailEndOpacityProp)
		obs_property_set_visible(tailEndOpacityProp, isTimed);
	if (tailOpacityFalloffProp)
		obs_property_set_visible(tailOpacityFalloffProp, isTimed);

	return true;
}

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
	std::string persistenceMode = PERSISTENCE_MODE_TIMED;
	float trailDurationSeconds = 1.0f;
	float trailThickness = 6.0f;
	float dotSize = 8.0f;
	std::string dotImagePath;
	float tailEndOpacity = 0.0f;
	float tailOpacityFalloff = 1.0f;
	gs_image_file_t dotImage{};
	bool dotImageLoaded = false;

	// Trail appearance properties
	float colorR = 0.0f;
	float colorG = 0.8f;
	float colorB = 1.0f;

	// Canvas mapping configuration
	std::string canvasMode = CANVAS_MODE_ALL_MONITORS;
	std::string selectedMonitorId;
	std::vector<MonitorInfo> availableMonitors;
	float mousepadDpi = 800.0f;
	bool mousepadDpiManual = false;
	float mousepadWidth = 250.0f;  // mm
	float mousepadHeight = 250.0f; // mm
	std::string boundaryMode = BOUNDARY_MODE_DELIMITED;
	bool showDpiOverlay = false;

	// Cached last cursor position for mousepad mode
	float lastCursorX = 0.0f;
	float lastCursorY = 0.0f;
	bool hasLastCursor = false;
	float mousepadNormX = 0.5f;
	float mousepadNormY = 0.5f;
	bool rawInputFallbackWarned = false;

	// Cached canvas bounds
	CanvasBounds currentCanvasBounds{1920.0f, 1080.0f, 0.0f, 0.0f};

	MouseTrailSource(obs_source_t *source) : source(source) {}
};

static void unload_custom_dot_image(MouseTrailSource *src)
{
	if (!src || !src->dotImageLoaded)
		return;

	obs_enter_graphics();
	gs_image_file_free(&src->dotImage);
	obs_leave_graphics();
	src->dotImageLoaded = false;
}

static void load_custom_dot_image(MouseTrailSource *src)
{
	if (!src)
		return;

	unload_custom_dot_image(src);

	if (src->dotImagePath.empty())
		return;

	gs_image_file_init(&src->dotImage, src->dotImagePath.c_str());
	if (!src->dotImage.loaded) {
		gs_image_file_free(&src->dotImage);
		obs_log(LOG_WARNING, "Failed to decode custom dot image: %s", src->dotImagePath.c_str());
		return;
	}

	obs_enter_graphics();
	gs_image_file_init_texture(&src->dotImage);
	obs_leave_graphics();

	if (!src->dotImage.texture) {
		obs_enter_graphics();
		gs_image_file_free(&src->dotImage);
		obs_leave_graphics();
		obs_log(LOG_WARNING, "Failed to create custom dot texture: %s", src->dotImagePath.c_str());
		return;
	}

	src->dotImageLoaded = true;
	obs_log(LOG_INFO, "Loaded custom dot image: %s", src->dotImagePath.c_str());
}

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

	const char *newPersistenceMode = obs_data_get_string(settings, SETTING_PERSISTENCE_MODE);
	if (newPersistenceMode && src->persistenceMode != newPersistenceMode) {
		src->persistenceMode = newPersistenceMode;
		settingsChanged = true;
	}

	double durationSeconds = obs_data_get_double(settings, SETTING_TRAIL_DURATION_SECONDS);
	if (durationSeconds >= 0.0)
		src->trailDurationSeconds = static_cast<float>(durationSeconds);

	if (src->trailDurationSeconds < 0.0f)
		src->trailDurationSeconds = 0.0f;
	src->trailDurationNs = static_cast<uint64_t>(src->trailDurationSeconds * 1e9);

	double thickness = obs_data_get_double(settings, SETTING_TRAIL_THICKNESS);
	if (thickness > 0.0)
		src->trailThickness = static_cast<float>(thickness);

	double dotSize = obs_data_get_double(settings, SETTING_DOT_SIZE);
	if (dotSize > 0.0)
		src->dotSize = static_cast<float>(dotSize);

	const char *newDotImagePath = obs_data_get_string(settings, SETTING_DOT_IMAGE_PATH);
	const std::string dotImagePath = newDotImagePath ? newDotImagePath : "";
	if (dotImagePath != src->dotImagePath) {
		src->dotImagePath = dotImagePath;
		load_custom_dot_image(src);
	}

	double endOpacity = obs_data_get_double(settings, SETTING_TAIL_END_OPACITY);
	src->tailEndOpacity = std::clamp(static_cast<float>(endOpacity), 0.0f, 1.0f);

	double opacityFalloff = obs_data_get_double(settings, SETTING_TAIL_OPACITY_FALLOFF);
	if (opacityFalloff > 0.0)
		src->tailOpacityFalloff = static_cast<float>(opacityFalloff);

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

	if (src->canvasMode == CANVAS_MODE_SPECIFIC_MONITOR &&
		!monitor_exists_by_id(src->availableMonitors, src->selectedMonitorId)) {
		const std::string fallbackId = get_fallback_monitor_id(src->availableMonitors);
		if (!fallbackId.empty() && src->selectedMonitorId != fallbackId) {
			src->selectedMonitorId = fallbackId;
			settingsChanged = true;
		}
	}

	// Mousepad settings
	src->mousepadDpiManual = obs_data_get_bool(settings, SETTING_MOUSEPAD_DPI_MANUAL);
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
		src->mousepadNormX = 0.5f;
		src->mousepadNormY = 0.5f;
		src->rawInputFallbackWarned = false;
		obs_log(LOG_INFO, "Canvas settings changed, trail cleared");
	}
}

static void *mouse_trail_create(obs_data_t *settings, obs_source_t *source)
{
	auto *src = new MouseTrailSource(source);
	enumerate_monitors(src->availableMonitors);

#ifdef _WIN32
	if (!raw_input_subsystem::init()) {
		obs_log(LOG_WARNING, "Raw input init failed. Mousepad mode will fallback to cursor-delta input.");
	}
#endif

	sync_runtime_status_settings(settings);

	obs_log(LOG_INFO, "mouse trail source created with %zu monitors", src->availableMonitors.size());
	mouse_trail_apply_settings(src, settings);
	return src;
}

static void mouse_trail_update(void *data, obs_data_t *settings)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	enumerate_monitors(src->availableMonitors);
	sync_runtime_status_settings(settings);
	mouse_trail_apply_settings(src, settings);
}

static bool clear_dot_image_button_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);

	if (!g_button_callback_source)
		return false;

	obs_data_t *settings = obs_source_get_settings(g_button_callback_source);
	if (settings) {
		obs_data_set_string(settings, SETTING_DOT_IMAGE_PATH, "");
		obs_source_update(g_button_callback_source, settings);
		obs_data_release(settings);
	}

	return true;
}

static obs_properties_t *mouse_trail_get_properties(void *data)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	// Store source pointer for button callback
	g_button_callback_source = src ? src->source : nullptr;
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *trailGroup = obs_properties_create();
	obs_property_t *modeProp = obs_properties_add_list(
		trailGroup,
		SETTING_MODE,
		obs_module_text("trail.mode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	if (modeProp) {
		obs_property_list_add_string(modeProp, obs_module_text("trail.mode.smoke"), MODE_SMOKE);
		obs_property_list_add_string(modeProp, obs_module_text("trail.mode.precision"), MODE_PRECISION);
	}
	obs_property_t *colorProp = obs_properties_add_color(trailGroup, SETTING_COLOR, obs_module_text("trail.color"));
	UNUSED_PARAMETER(colorProp);

	obs_property_t *persistenceProp = obs_properties_add_list(
		trailGroup,
		SETTING_PERSISTENCE_MODE,
		obs_module_text("trail.persistence"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	if (persistenceProp) {
		obs_property_list_add_string(persistenceProp, obs_module_text("trail.persistence.timed"), PERSISTENCE_MODE_TIMED);
		obs_property_list_add_string(persistenceProp, obs_module_text("trail.persistence.forever"), PERSISTENCE_MODE_FOREVER);
		obs_property_list_add_string(persistenceProp, obs_module_text("trail.persistence.laser"), PERSISTENCE_MODE_LASER);
		obs_property_set_modified_callback(persistenceProp, persistence_mode_modified);
	}

	obs_property_t *durationProp = obs_properties_add_float_slider(
		trailGroup,
		SETTING_TRAIL_DURATION_SECONDS,
		obs_module_text("trail.duration"),
		0.0,
		30.0,
		0.05);
	UNUSED_PARAMETER(durationProp);

	obs_property_t *thicknessProp = obs_properties_add_float_slider(
		trailGroup,
		SETTING_TRAIL_THICKNESS,
		obs_module_text("trail.thickness"),
		1.0,
		40.0,
		0.5);
	UNUSED_PARAMETER(thicknessProp);

	obs_property_t *dotSizeProp = obs_properties_add_float_slider(
		trailGroup,
		SETTING_DOT_SIZE,
		obs_module_text("trail.dot_size"),
		1.0,
		40.0,
		0.5);
	UNUSED_PARAMETER(dotSizeProp);

	obs_property_t *dotImageProp = obs_properties_add_path(
		trailGroup,
		SETTING_DOT_IMAGE_PATH,
		obs_module_text("trail.dot_image"),
		OBS_PATH_FILE,
		DOT_IMAGE_FILTER,
		nullptr);
	UNUSED_PARAMETER(dotImageProp);

	obs_property_t *clearDotImageProp = obs_properties_add_button(
		trailGroup,
		"clear_dot_image",
		obs_module_text("trail.dot_image_clear"),
		clear_dot_image_button_clicked);
	UNUSED_PARAMETER(clearDotImageProp);

	obs_property_t *tailEndOpacityProp = obs_properties_add_float_slider(
		trailGroup,
		SETTING_TAIL_END_OPACITY,
		obs_module_text("trail.tail_end_opacity"),
		0.0,
		1.0,
		0.01);
	UNUSED_PARAMETER(tailEndOpacityProp);

	obs_property_t *tailOpacityFalloffProp = obs_properties_add_float_slider(
		trailGroup,
		SETTING_TAIL_OPACITY_FALLOFF,
		obs_module_text("trail.tail_opacity_falloff"),
		0.2,
		3.0,
		0.05);
	UNUSED_PARAMETER(tailOpacityFalloffProp);
	obs_properties_add_group(props, "trail_group", obs_module_text("group.trail"), OBS_GROUP_NORMAL, trailGroup);

	obs_properties_t *canvasGroup = obs_properties_create();

	// Canvas mode selection
	obs_property_t *canvasModeProp = obs_properties_add_list(
		canvasGroup,
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
		obs_property_set_modified_callback(canvasModeProp, canvas_mode_modified);
	}

	// Monitor selection dropdown
	obs_property_t *monitorProp = obs_properties_add_list(
		canvasGroup,
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
	obs_property_t *dpiManualProp = obs_properties_add_bool(
		canvasGroup,
		SETTING_MOUSEPAD_DPI_MANUAL,
		obs_module_text("mousepad.dpi_manual"));
	if (dpiManualProp)
		obs_property_set_modified_callback(dpiManualProp, dpi_settings_modified);
	UNUSED_PARAMETER(dpiManualProp);

	obs_property_t *dpiProp = obs_properties_add_float(canvasGroup, SETTING_MOUSEPAD_DPI, obs_module_text("mousepad.dpi"),
		100.0, 3200.0, 1.0);
	if (dpiProp)
		obs_property_set_modified_callback(dpiProp, dpi_settings_modified);
	UNUSED_PARAMETER(dpiProp);

	obs_property_t *dpiEffectiveProp = obs_properties_add_text(
		canvasGroup,
		SETTING_MOUSEPAD_DPI_EFFECTIVE,
		obs_module_text("mousepad.dpi_effective"),
		OBS_TEXT_DEFAULT);
	if (dpiEffectiveProp)
		obs_property_set_enabled(dpiEffectiveProp, false);
	UNUSED_PARAMETER(dpiEffectiveProp);

	// Mousepad width (mm)
	obs_property_t *padWProp = obs_properties_add_float(canvasGroup, SETTING_MOUSEPAD_WIDTH, obs_module_text("mousepad.width"),
		1.0, 1000.0, 0.1);
	UNUSED_PARAMETER(padWProp);

	// Mousepad height (mm)
	obs_property_t *padHProp = obs_properties_add_float(canvasGroup, SETTING_MOUSEPAD_HEIGHT, obs_module_text("mousepad.height"),
		1.0, 1000.0, 0.1);
	UNUSED_PARAMETER(padHProp);

	obs_property_t *rawStatusProp = obs_properties_add_bool(
		canvasGroup,
		SETTING_MOUSEPAD_RAW_INPUT_ACTIVE,
		obs_module_text("mousepad.raw_input_active"));
	if (rawStatusProp)
		obs_property_set_enabled(rawStatusProp, false);
	obs_properties_add_group(props, "canvas_group", obs_module_text("group.canvas"), OBS_GROUP_NORMAL, canvasGroup);

	obs_properties_t *boundaryGroup = obs_properties_create();

	obs_properties_add_bool(props, SETTING_SHOW_DPI_OVERLAY, obs_module_text("mousepad.show_dpi_overlay"));

	// Boundary mode selection
	obs_property_t *boundaryModeProp = obs_properties_add_list(
		boundaryGroup,
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
	obs_properties_add_group(props, "boundary_group", obs_module_text("group.boundary"), OBS_GROUP_NORMAL, boundaryGroup);

	if (src) {
		obs_data_t *previewSettings = obs_source_get_settings(src->source);
		if (previewSettings) {
			sync_runtime_status_settings(previewSettings);
			sync_effective_dpi_display(previewSettings);
			canvas_mode_modified(props, nullptr, previewSettings);
			persistence_mode_modified(props, nullptr, previewSettings);
			obs_data_release(previewSettings);
		}
	}

	return props;
}

static void mouse_trail_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_MODE, MODE_SMOKE);
	obs_data_set_default_int(settings, SETTING_COLOR, 0x00CCFF);
	obs_data_set_default_string(settings, SETTING_PERSISTENCE_MODE, PERSISTENCE_MODE_TIMED);
	obs_data_set_default_double(settings, SETTING_TRAIL_DURATION_SECONDS, 1.0);
	obs_data_set_default_double(settings, SETTING_TRAIL_THICKNESS, 6.0);
	obs_data_set_default_double(settings, SETTING_DOT_SIZE, 8.0);
	obs_data_set_default_string(settings, SETTING_DOT_IMAGE_PATH, "");
	obs_data_set_default_double(settings, SETTING_TAIL_END_OPACITY, 0.0);
	obs_data_set_default_double(settings, SETTING_TAIL_OPACITY_FALLOFF, 1.0);
	obs_data_set_default_string(settings, SETTING_CANVAS_MODE, CANVAS_MODE_ALL_MONITORS);
	obs_data_set_default_string(settings, SETTING_SELECTED_MONITOR, "");
	obs_data_set_default_bool(settings, SETTING_MOUSEPAD_DPI_MANUAL, true);
	obs_data_set_default_double(settings, SETTING_MOUSEPAD_DPI, 800.0);
	obs_data_set_default_string(settings, SETTING_MOUSEPAD_DPI_EFFECTIVE, "");
	obs_data_set_default_double(settings, SETTING_MOUSEPAD_WIDTH, 250.0);
	obs_data_set_default_double(settings, SETTING_MOUSEPAD_HEIGHT, 250.0);
	obs_data_set_default_bool(settings, SETTING_MOUSEPAD_RAW_INPUT_ACTIVE, false);
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

	// Enumerate individual monitors
	MONITORINFOEXA miex;
	ZeroMemory(&miex, sizeof(miex));
	miex.cbSize = sizeof(miex);

	auto enumProc = [](HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
		UNUSED_PARAMETER(hdcMonitor);
		auto *monitors = reinterpret_cast<std::vector<MonitorInfo>*>(dwData);

		MONITORINFOEXA miex;
		ZeroMemory(&miex, sizeof(miex));
		miex.cbSize = sizeof(miex);
		if (!GetMonitorInfoA(hMonitor, reinterpret_cast<MONITORINFO *>(&miex)))
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

	obs_log(LOG_INFO, "Enumerated %zu individual monitors", outMonitors.size());
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

		for (const auto &monitor : monitors) {
			if (monitor.isPrimary) {
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
		if (normX < 0.0f || normX > 1.0f || normY < 0.0f || normY > 1.0f)
			return {0.5f, 0.5f};
		return {normX, normY};
	}

	return {normX, normY};
}

static bool boundary_transition_is_teleport(float normX, float normY, const char *boundaryMode)
{
	if (!boundaryMode)
		return false;

	const bool outOfBounds = (normX < 0.0f || normX > 1.0f || normY < 0.0f || normY > 1.0f);
	if (!outOfBounds)
		return false;

	return strcmp(boundaryMode, BOUNDARY_MODE_INFINITE_WRAP) == 0 ||
		strcmp(boundaryMode, BOUNDARY_MODE_INFINITE_RECENTER) == 0;
}

static void mouse_trail_destroy(void *data)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	unload_custom_dot_image(src);

#ifdef _WIN32
	raw_input_subsystem::shutdown();
#endif
	delete src;
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
	bool breakBeforePoint = false;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

#ifdef _WIN32
	if (strcmp(src->canvasMode.c_str(), CANVAS_MODE_MOUSEPAD) == 0) {
		float deltaX = 0.0f;
		float deltaY = 0.0f;
		const bool rawAvailable = raw_input_subsystem::is_available();
		bool haveDelta = false;

		if (rawAvailable) {
			haveDelta = raw_input_subsystem::consume_delta(deltaX, deltaY);
			// Keep cursor fallback state reset while raw input is active to avoid stale-delta jumps.
			src->hasLastCursor = false;
		}

		if (!rawAvailable) {
			POINT mousePos;
			if (GetCursorPos(&mousePos)) {
				if (!src->hasLastCursor) {
					src->lastCursorX = static_cast<float>(mousePos.x);
					src->lastCursorY = static_cast<float>(mousePos.y);
					src->hasLastCursor = true;
				} else {
					deltaX = static_cast<float>(mousePos.x) - src->lastCursorX;
					deltaY = static_cast<float>(mousePos.y) - src->lastCursorY;
					haveDelta = (std::abs(deltaX) > 0.001f || std::abs(deltaY) > 0.001f);
					src->lastCursorX = static_cast<float>(mousePos.x);
					src->lastCursorY = static_cast<float>(mousePos.y);
					if (!src->rawInputFallbackWarned && !raw_input_subsystem::is_available()) {
						src->rawInputFallbackWarned = true;
						obs_log(LOG_WARNING, "Raw input unavailable, using cursor-delta fallback for mousepad mode.");
					}
				}
			}
		}

		if (haveDelta) {
			const float effectiveDpi = get_effective_mouse_dpi(src->mousepadDpiManual, src->mousepadDpi);
			float mmPerCount = 25.4f / effectiveDpi;
			float deltaMm_X = deltaX * mmPerCount;
			float deltaMm_Y = deltaY * mmPerCount;

			float normDelta_X = deltaMm_X / src->mousepadWidth;
			float normDelta_Y = deltaMm_Y / src->mousepadHeight;

			float normX = src->mousepadNormX + normDelta_X;
			float normY = src->mousepadNormY + normDelta_Y;
			breakBeforePoint = boundary_transition_is_teleport(normX, normY, src->boundaryMode.c_str());

			auto [boundaryX, boundaryY] = apply_boundary_mode(normX, normY, src->boundaryMode.c_str());
			src->mousepadNormX = boundaryX;
			src->mousepadNormY = boundaryY;

			mouseX = boundaryX * ovi.base_width;
			mouseY = boundaryY * ovi.base_height;
			haveMappedCursor = true;
		}
	} else {
		src->hasLastCursor = false;
		POINT mousePos;
		if (GetCursorPos(&mousePos)) {
			auto [normX, normY] = map_cursor_to_canvas(
				static_cast<float>(mousePos.x),
				static_cast<float>(mousePos.y),
				src->canvasMode.c_str(),
				src->selectedMonitorId.c_str(),
				src->availableMonitors,
				src->currentCanvasBounds
			);

			breakBeforePoint = boundary_transition_is_teleport(normX, normY, src->boundaryMode.c_str());

			auto [boundaryX, boundaryY] = apply_boundary_mode(normX, normY, src->boundaryMode.c_str());
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
		if (src->persistenceMode == PERSISTENCE_MODE_LASER) {
			src->trailManager.clear();
		}
		src->trailManager.addPoint(mouseX, mouseY, breakBeforePoint);
	}

	// 2. Clean up aged points
	if (src->persistenceMode == PERSISTENCE_MODE_TIMED)
		src->trailManager.cleanup(src->trailDurationNs);

	// 3. Move points upward simulating anti-gravity (frame-rate independent)
	if (src->smokeMode && src->persistenceMode != PERSISTENCE_MODE_LASER)
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
	const bool showDpiOverlay = src->showDpiOverlay;
	if (points.empty() && !showDpiOverlay)
		return;

	static uint64_t lastRenderLogNs = 0;
	uint64_t nowNs = os_gettime_ns();
	if (nowNs - lastRenderLogNs > 2000000000ULL) {
		lastRenderLogNs = nowNs;
		if (points.size() >= 2) {
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
	const bool hasPoints = !points.empty();
	const Point head = hasPoints ? points.back() : Point{};
	const bool useCustomDot = src->dotImageLoaded && src->dotImage.texture != nullptr;

	// Begin rendering techniques and passes
	size_t passes = gs_technique_begin(tech);
	for (size_t pass = 0; pass < passes; pass++) {
		gs_technique_begin_pass(tech, pass);

		const bool drawTail = src->persistenceMode != PERSISTENCE_MODE_LASER;
		if (drawTail && points.size() >= 2) {
			const float thickness = src->trailThickness;
			for (size_t i = 1; i < points.size(); i++) {
				const Point &a = points[i - 1];
				const Point &b = points[i];
				if (b.breakBefore)
					continue;

				float dx = b.x - a.x;
				float dy = b.y - a.y;
				float len = std::sqrt(dx * dx + dy * dy);
				if (len < 0.001f)
					continue;

				float nx = -dy / len;
				float ny = dx / len;
				float hx = nx * (thickness * 0.5f);
				float hy = ny * (thickness * 0.5f);

				float alpha = 1.0f;
					if (src->persistenceMode == PERSISTENCE_MODE_TIMED && src->trailDurationNs > 0) {
						uint64_t age = (currentTime >= b.timestamp) ? (currentTime - b.timestamp) : 0;
						float ageNorm = static_cast<float>(age) / static_cast<float>(src->trailDurationNs);
						ageNorm = std::clamp(ageNorm, 0.0f, 1.0f);
						float fadeShape = std::pow(ageNorm, std::max(0.01f, src->tailOpacityFalloff));
						alpha = 1.0f - (1.0f - src->tailEndOpacity) * fadeShape;
					}
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

		if (!useCustomDot && hasPoints) {
			const float halfDot = src->dotSize * 0.5f;
			struct vec4 dotColor;
			vec4_set(&dotColor, src->colorR, src->colorG, src->colorB, 1.0f);
			gs_effect_set_vec4(colorParam, &dotColor);
			gs_render_start(true);
			gs_vertex2f(head.x - halfDot, head.y - halfDot);
			gs_vertex2f(head.x + halfDot, head.y - halfDot);
			gs_vertex2f(head.x - halfDot, head.y + halfDot);
			gs_vertex2f(head.x + halfDot, head.y + halfDot);
			gs_render_stop(GS_TRISTRIP);
		}

		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);

	if (useCustomDot && hasPoints) {
		gs_effect_t *dotEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		if (dotEffect) {
			gs_eparam_t *imageParam = gs_effect_get_param_by_name(dotEffect, "image");
			if (imageParam) {
				gs_effect_set_texture(imageParam, src->dotImage.texture);

				const float size = src->dotSize;
				const float drawX = head.x - (size * 0.5f);
				const float drawY = head.y - (size * 0.5f);

				const float texW = (src->dotImage.cx > 0) ? static_cast<float>(src->dotImage.cx) : 1.0f;
				const float texH = (src->dotImage.cy > 0) ? static_cast<float>(src->dotImage.cy) : 1.0f;

				gs_blend_state_push();
				gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
				gs_matrix_push();
				gs_matrix_translate3f(drawX, drawY, 0.0f);
				gs_matrix_scale3f(size / texW, size / texH, 1.0f);
				while (gs_effect_loop(dotEffect, "Draw")) {
					gs_draw_sprite(src->dotImage.texture, 0, src->dotImage.cx, src->dotImage.cy);
				}
				gs_matrix_pop();
				gs_blend_state_pop();
			}
		}
	}

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
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
};
