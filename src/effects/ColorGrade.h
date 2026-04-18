/**
 * @file
 * @brief Header file for ColorGrade effect
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef OPENSHOT_COLOR_GRADE_EFFECT_H
#define OPENSHOT_COLOR_GRADE_EFFECT_H

#include "../EffectBase.h"
#include "../Frame.h"
#include "../Json.h"
#include "../KeyFrame.h"
#include "ColorMap.h"

#include <QColor>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace openshot
{
	/**
	 * @brief A compact rich curve payload used by the ColorGrade effect.
	 *
	 * The JSON form is:
	 * {
	 *   "points": [
	 *     {"x": 0.0, "y": 0.0},
	 *     {"x": 1.0, "y": 1.0}
	 *   ]
	 * }
	 */
	struct ColorGradeCurveData {
		bool enabled;
		std::vector<std::pair<float, float>> points;

		ColorGradeCurveData();
		Json::Value JsonValue() const;
		void SetJsonValue(const Json::Value& root);
		float Sample(float input) const;
		std::string Summary() const;
	};

	/**
	 * @brief Wheel payload for a tonal region.
	 *
	 * The JSON form is:
	 * {
	 *   "color": "#RRGGBB",
	 *   "amount": 0.0,
	 *   "luma": 0.0
	 * }
	 */
	struct ColorGradeWheelEntry {
		QColor color;
		float amount;
		float luma;

		ColorGradeWheelEntry();
		Json::Value JsonValue() const;
		void SetJsonValue(const Json::Value& root);
	};

	/**
	 * @brief All wheel controls for ColorGrade.
	 */
	struct ColorGradeWheelsData {
		bool enabled;
		ColorGradeWheelEntry global;
		ColorGradeWheelEntry shadows;
		ColorGradeWheelEntry midtones;
		ColorGradeWheelEntry highlights;

		ColorGradeWheelsData();
		Json::Value JsonValue() const;
		void SetJsonValue(const Json::Value& root);
		std::string Summary() const;
	};

	/**
	 * @brief A unified beginner-friendly color grading effect.
	 *
	 * V1 intentionally combines simple corrections, rich curves, tonal wheels,
	 * and LUT support into a single effect payload so the editor can expose one
	 * coherent grading workflow.
	 */
	class ColorGrade : public EffectBase
	{
	private:
		ColorMap lut_effect;
		std::string lut_path;
		bool lut_dirty;

		void init_effect_details();
		void sync_lut_effect();

		static float Clamp01(float value);

	public:
		Keyframe temperature;
		Keyframe tint;
		Keyframe exposure;
		Keyframe contrast;
		Keyframe highlights;
		Keyframe shadows;
		Keyframe saturation;
		Keyframe vibrance;
		Keyframe mix;
		Keyframe lut_intensity;

		ColorGradeWheelsData wheels;
		ColorGradeCurveData curve_master;
		ColorGradeCurveData curve_red;
		ColorGradeCurveData curve_green;
		ColorGradeCurveData curve_blue;

		ColorGrade();

		std::shared_ptr<openshot::Frame> GetFrame(int64_t frame_number) override {
			return GetFrame(std::make_shared<openshot::Frame>(), frame_number);
		}
		std::shared_ptr<openshot::Frame> GetFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number) override;

		std::string Json() const override;
		Json::Value JsonValue() const override;
		void SetJson(const std::string value) override;
		void SetJsonValue(const Json::Value root) override;
		std::string PropertiesJSON(int64_t requested_frame) const override;
	};
}

#endif
