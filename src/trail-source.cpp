#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <algorithm> // For std::clamp
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include "plugin-support.h"
#include "trail-manager.hpp"

static constexpr const char *SETTING_MODE = "trail_mode";
static constexpr const char *SETTING_COLOR = "trail_color";
static constexpr const char *MODE_SMOKE = "smoke";
static constexpr const char *MODE_PRECISION = "precision";

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

	const char *mode = obs_data_get_string(settings, SETTING_MODE);
	src->smokeMode = (mode && strcmp(mode, MODE_PRECISION) != 0);

	const uint32_t color = static_cast<uint32_t>(obs_data_get_int(settings, SETTING_COLOR));
	// OBS color property stores packed color as BBGGRR.
	src->colorR = static_cast<float>(color & 0xFF) / 255.0f;
	src->colorG = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
	src->colorB = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
}

static void *mouse_trail_create(obs_data_t *settings, obs_source_t *source)
{
	auto *src = new MouseTrailSource(source);
	obs_log(LOG_INFO, "mouse trail source created");
	mouse_trail_apply_settings(src, settings);
	return src;
}

static void mouse_trail_update(void *data, obs_data_t *settings)
{
	auto *src = static_cast<MouseTrailSource*>(data);
	mouse_trail_apply_settings(src, settings);
}

static obs_properties_t *mouse_trail_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
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

	return props;
}

static void mouse_trail_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_MODE, MODE_SMOKE);
	obs_data_set_default_int(settings, SETTING_COLOR, 0x00CCFF);
}

static void mouse_trail_destroy(void *data)
{
	delete static_cast<MouseTrailSource*>(data);
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

#ifdef _WIN32
	POINT mousePos;
	if (GetCursorPos(&mousePos)) {
		// Map desktop coordinates to OBS base canvas coordinates so the source always
		// renders inside its own bounds, even with multi-monitor virtual desktop offsets.
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
			int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
			int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
			int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

			if (virtualWidth > 0 && virtualHeight > 0) {
				float normX = static_cast<float>(mousePos.x - virtualLeft) / static_cast<float>(virtualWidth);
				float normY = static_cast<float>(mousePos.y - virtualTop) / static_cast<float>(virtualHeight);

				normX = std::clamp(normX, 0.0f, 1.0f);
				normY = std::clamp(normY, 0.0f, 1.0f);

				mouseX = normX * static_cast<float>(ovi.base_width);
				mouseY = normY * static_cast<float>(ovi.base_height);
				haveMappedCursor = true;
			}
		}

		if (!haveMappedCursor) {
			mouseX = static_cast<float>(mousePos.x);
			mouseY = static_cast<float>(mousePos.y);
		}
	}
#else
	// macOS/Linux implementation helper placeholder:
	// E.g. on macOS, retrieve pointer via CGEventSourceGetPixmapCursorPosition(kCGEventSourceStateCombinedSessionState)
#endif

	// 1. Add point to queue
	src->trailManager.addPoint(mouseX, mouseY);

	// 2. Clean up aged points
	src->trailManager.cleanup(src->trailDurationNs);

	// 3. Move points upward simulating anti-gravity (frame-rate independent)
	if (src->smokeMode)
		src->trailManager.applyGravity(src->gravityFactor, seconds);

	static uint64_t lastTickLogNs = 0;
	uint64_t now = os_gettime_ns();
	if (now - lastTickLogNs > 2000000000ULL) {
		lastTickLogNs = now;
		obs_log(LOG_INFO, "tick: mapped cursor x=%.1f y=%.1f", mouseX, mouseY);
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
	if (points.size() < 2) return;

	static uint64_t lastRenderLogNs = 0;
	uint64_t nowNs = os_gettime_ns();
	if (nowNs - lastRenderLogNs > 2000000000ULL) {
		lastRenderLogNs = nowNs;
		obs_log(LOG_INFO, "render: points=%d first=(%.1f, %.1f) last=(%.1f, %.1f)", (int)points.size(),
			points.front().x, points.front().y, points.back().x, points.back().y);
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

		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);

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
