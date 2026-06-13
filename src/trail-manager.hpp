#pragma once

#include <deque>
#include <mutex>
#include <cstdint>
#include <util/platform.h> // Provides os_gettime_ns() in the OBS Studio API

/**
 * @brief Represents a single point in the mouse trail.
 */
struct Point {
	float x;
	float y;
	uint64_t timestamp; // Timestamp in nanoseconds (using os_gettime_ns())
};

/**
 * @brief Manages the mouse trail points, handles thread safety, cleanup, and anti-gravity logic.
 */
class TrailManager {
private:
	std::deque<Point> trail;
	mutable std::mutex trailMutex;
	uint64_t lastUpdateTime = 0;

public:
	TrailManager() {
		lastUpdateTime = os_gettime_ns();
	}

	/**
	 * @brief Adds a new cursor point to the trail.
	 * 
	 * @param x The horizontal coordinate of the cursor.
	 * @param y The vertical coordinate of the cursor.
	 */
	void addPoint(float x, float y) {
		uint64_t currentTime = os_gettime_ns();
		std::lock_guard<std::mutex> lock(trailMutex);
		trail.push_back({x, y, currentTime});
	}

	/**
	 * @brief Cleans up points that have exceeded the specified duration.
	 * 
	 * @param durationNs The lifetime of a trail point in nanoseconds.
	 */
	void cleanup(uint64_t durationNs) {
		uint64_t currentTime = os_gettime_ns();
		std::lock_guard<std::mutex> lock(trailMutex);

		while (!trail.empty()) {
			// Prevent underflow if system time adjusts or there is clock jitter
			if (currentTime >= trail.front().timestamp) {
				if (currentTime - trail.front().timestamp > durationNs) {
					trail.pop_front();
					continue;
				}
			}
			break;
		}
	}

	/**
	 * @brief Applies an anti-gravity effect to the trail points based on their age.
	 *        This version automatically calculates the delta-time (dt) since the last call.
	 * 
	 * @param gravityFactor The acceleration rate at which older points move upward.
	 */
	void applyGravity(float gravityFactor) {
		uint64_t currentTime = os_gettime_ns();
		std::lock_guard<std::mutex> lock(trailMutex);

		if (trail.empty()) {
			lastUpdateTime = currentTime;
			return;
		}

		double dt = 0.0;
		if (lastUpdateTime > 0 && currentTime > lastUpdateTime) {
			dt = static_cast<double>(currentTime - lastUpdateTime) / 1'000'000'000.0;
		}
		lastUpdateTime = currentTime;

		if (dt <= 0.0) {
			return;
		}

		applyGravityInternal(gravityFactor, static_cast<float>(dt), currentTime);
	}

	/**
	 * @brief Applies an anti-gravity effect using an externally supplied delta-time (dt).
	 *        This is extremely useful when updating inside OBS's obs_source_info::video_tick callback.
	 * 
	 * @param gravityFactor The acceleration rate at which older points move upward.
	 * @param dt The delta-time since the last tick in seconds.
	 */
	void applyGravity(float gravityFactor, float dt) {
		uint64_t currentTime = os_gettime_ns();
		std::lock_guard<std::mutex> lock(trailMutex);

		if (dt <= 0.0f || trail.empty()) {
			return;
		}

		applyGravityInternal(gravityFactor, dt, currentTime);
	}

	/**
	 * @brief Gets a copy of the trail points for rendering.
	 *        Locks the mutex to ensure safety if another thread is modifying the trail.
	 * 
	 * @return std::deque<Point> A copy of the trail.
	 */
	std::deque<Point> getPoints() const {
		std::lock_guard<std::mutex> lock(trailMutex);
		return trail;
	}

	/**
	 * @brief Clears all points in the trail.
	 */
	void clear() {
		std::lock_guard<std::mutex> lock(trailMutex);
		trail.clear();
	}

private:
	/**
	 * @brief Internal helper to update point positions. Mutex must already be locked.
	 */
	void applyGravityInternal(float gravityFactor, float dt, uint64_t currentTime) {
		for (auto &point : trail) {
			if (currentTime > point.timestamp) {
				// Calculate the age of the point in seconds
				double age = static_cast<double>(currentTime - point.timestamp) / 1'000'000'000.0;
				
				// In OBS's coordinate system, y increases downwards, so subtracting shifts it upward (anti-gravity).
				// We scale the shift by both the point's age and the frame's delta-time (dt) to keep it frame-rate independent.
				point.y -= static_cast<float>(gravityFactor * age * dt);
			}
		}
	}
};
