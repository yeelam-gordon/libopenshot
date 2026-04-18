/**
 * @file
 * @brief Source file for ColorGrade effect
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ColorGrade.h"
#include "Exceptions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

using namespace openshot;

namespace {
constexpr float kInv255 = 1.0f / 255.0f;

static float clamp01(float value) {
	return std::max(0.0f, std::min(1.0f, value));
}

static int clampByte(float value) {
	if (value <= 0.0f)
		return 0;
	if (value >= 255.0f)
		return 255;
	return static_cast<int>(std::round(value));
}

static float smooth_midtones(float luma) {
	const float centered = std::abs(luma - 0.5f) * 2.0f;
	return clamp01(1.0f - centered * centered);
}

static std::string color_to_hex(const QColor& color) {
	return color.name(QColor::HexRgb).toStdString();
}

static std::array<float, 256> build_curve_lut(const ColorGradeCurveData& curve) {
	std::array<float, 256> lut{};
	for (size_t i = 0; i < lut.size(); ++i) {
		lut[i] = curve.Sample(static_cast<float>(i) * kInv255);
	}
	return lut;
}

static float sample_curve_lut(const std::array<float, 256>& lut, float value) {
	return lut[clampByte(clamp01(value) * 255.0f)];
}
}

ColorGradeCurveData::ColorGradeCurveData()
	: enabled(true), points{{0.0f, 0.0f}, {1.0f, 1.0f}} {}

Json::Value ColorGradeCurveData::JsonValue() const {
	Json::Value root(Json::objectValue);
	root["enabled"] = enabled;
	root["points"] = Json::Value(Json::arrayValue);
	for (const auto& point : points) {
		Json::Value item(Json::objectValue);
		item["x"] = point.first;
		item["y"] = point.second;
		root["points"].append(item);
	}
	return root;
}

void ColorGradeCurveData::SetJsonValue(const Json::Value& root) {
	if (!root["enabled"].isNull())
		enabled = root["enabled"].asBool();

	std::vector<std::pair<float, float>> parsed;
	const Json::Value& json_points = root["points"];
	if (json_points.isArray()) {
		for (const auto& point : json_points) {
			if (!point["x"].isNull() && !point["y"].isNull()) {
				parsed.emplace_back(
					clamp01(point["x"].asFloat()),
					clamp01(point["y"].asFloat()));
			}
		}
	}
	if (parsed.empty()) {
		parsed = {{0.0f, 0.0f}, {1.0f, 1.0f}};
	}
	std::sort(parsed.begin(), parsed.end(), [](const auto& lhs, const auto& rhs) {
		return lhs.first < rhs.first;
	});
	if (parsed.front().first > 0.0f) {
		parsed.insert(parsed.begin(), {0.0f, parsed.front().second});
	} else {
		parsed.front().first = 0.0f;
	}
	if (parsed.back().first < 1.0f) {
		parsed.push_back({1.0f, parsed.back().second});
	} else {
		parsed.back().first = 1.0f;
	}
	points.swap(parsed);
}

float ColorGradeCurveData::Sample(float input) const {
	if (!enabled)
		return clamp01(input);
	const float x = clamp01(input);
	if (points.empty())
		return x;
	if (x <= points.front().first)
		return points.front().second;
	if (x >= points.back().first)
		return points.back().second;

	for (size_t i = 1; i < points.size(); ++i) {
		const auto& left = points[i - 1];
		const auto& right = points[i];
		if (x <= right.first) {
			const float span = std::max(0.00001f, right.first - left.first);
			const float t = (x - left.first) / span;
			return left.second + ((right.second - left.second) * t);
		}
	}
	return points.back().second;
}

std::string ColorGradeCurveData::Summary() const {
	std::ostringstream ss;
	if (!enabled)
		ss << "Disabled, ";
	ss << points.size() << " pts";
	return ss.str();
}

ColorGradeWheelEntry::ColorGradeWheelEntry()
	: color(Qt::white), amount(0.0f), luma(0.0f) {}

Json::Value ColorGradeWheelEntry::JsonValue() const {
	Json::Value root(Json::objectValue);
	root["color"] = color_to_hex(color);
	root["amount"] = amount;
	root["luma"] = luma;
	return root;
}

void ColorGradeWheelEntry::SetJsonValue(const Json::Value& root) {
	if (!root["color"].isNull()) {
		const QColor parsed(QString::fromStdString(root["color"].asString()));
		if (parsed.isValid())
			color = parsed;
	}
	if (!root["amount"].isNull())
		amount = std::max(-1.0f, std::min(1.0f, root["amount"].asFloat()));
	if (!root["luma"].isNull())
		luma = std::max(-1.0f, std::min(1.0f, root["luma"].asFloat()));
}

ColorGradeWheelsData::ColorGradeWheelsData()
	: enabled(true) {}

Json::Value ColorGradeWheelsData::JsonValue() const {
	Json::Value root(Json::objectValue);
	root["enabled"] = enabled;
	root["global"] = global.JsonValue();
	root["shadows"] = shadows.JsonValue();
	root["midtones"] = midtones.JsonValue();
	root["highlights"] = highlights.JsonValue();
	return root;
}

void ColorGradeWheelsData::SetJsonValue(const Json::Value& root) {
	if (!root["enabled"].isNull())
		enabled = root["enabled"].asBool();
	if (!root["global"].isNull())
		global.SetJsonValue(root["global"]);
	if (!root["shadows"].isNull())
		shadows.SetJsonValue(root["shadows"]);
	if (!root["midtones"].isNull())
		midtones.SetJsonValue(root["midtones"]);
	if (!root["highlights"].isNull())
		highlights.SetJsonValue(root["highlights"]);
}

std::string ColorGradeWheelsData::Summary() const {
	return enabled ? "Global / Shadows / Midtones / Highlights"
	               : "Disabled";
}

ColorGrade::ColorGrade()
	: temperature(0.0),
	  tint(0.0),
	  exposure(0.0),
	  contrast(0.0),
	  highlights(0.0),
	  shadows(0.0),
	  saturation(1.0),
	  vibrance(0.0),
	  mix(1.0),
	  lut_intensity(1.0),
	  lut_path(""),
	  lut_dirty(true)
{
	init_effect_details();
}

void ColorGrade::init_effect_details() {
	InitEffectInfo();
	info.class_name = "ColorGrade";
	info.name = "Color Grade";
	info.description = "Unified color grading effect with curves, wheels and LUT support.";
	info.has_audio = false;
	info.has_video = true;
}

float ColorGrade::Clamp01(float value) {
	return clamp01(value);
}

void ColorGrade::sync_lut_effect() {
	if (!lut_dirty)
		return;

	Json::Value payload(Json::objectValue);
	payload["lut_path"] = lut_path;
	payload["intensity"] = lut_intensity.JsonValue();
	payload["intensity_r"] = Keyframe(1.0).JsonValue();
	payload["intensity_g"] = Keyframe(1.0).JsonValue();
	payload["intensity_b"] = Keyframe(1.0).JsonValue();
	lut_effect.SetJsonValue(payload);
	lut_dirty = false;
}

std::shared_ptr<openshot::Frame> ColorGrade::GetFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number) {
	std::shared_ptr<QImage> frame_image = frame->GetImage();
	if (!frame_image)
		return frame;

	const float temperature_value = std::max(-1.0f, std::min(1.0f, static_cast<float>(temperature.GetValue(frame_number))));
	const float tint_value = std::max(-1.0f, std::min(1.0f, static_cast<float>(tint.GetValue(frame_number))));
	const float exposure_value = std::max(-4.0f, std::min(4.0f, static_cast<float>(exposure.GetValue(frame_number))));
	const float contrast_value = std::max(-1.0f, std::min(2.0f, static_cast<float>(contrast.GetValue(frame_number))));
	const float highlights_value = std::max(-1.0f, std::min(1.0f, static_cast<float>(highlights.GetValue(frame_number))));
	const float shadows_value = std::max(-1.0f, std::min(1.0f, static_cast<float>(shadows.GetValue(frame_number))));
	const float saturation_value = std::max(0.0f, std::min(4.0f, static_cast<float>(saturation.GetValue(frame_number))));
	const float vibrance_value = std::max(-1.0f, std::min(1.0f, static_cast<float>(vibrance.GetValue(frame_number))));
	const float mix_value = std::max(0.0f, std::min(1.0f, static_cast<float>(mix.GetValue(frame_number))));
	const float exposure_gain = std::pow(2.0f, exposure_value);
	const float contrast_factor = std::max(0.0f, 1.0f + contrast_value);
	const std::array<float, 256> curve_master_lut = build_curve_lut(curve_master);
	const std::array<float, 256> curve_red_lut = build_curve_lut(curve_red);
	const std::array<float, 256> curve_green_lut = build_curve_lut(curve_green);
	const std::array<float, 256> curve_blue_lut = build_curve_lut(curve_blue);

	static const std::array<float, 256> inv_alpha = [] {
		std::array<float, 256> lut{};
		lut[0] = 0.0f;
		for (int i = 1; i < 256; ++i)
			lut[i] = 255.0f / static_cast<float>(i);
		return lut;
	}();

	const auto wheel_bias = [](const ColorGradeWheelEntry& wheel, float weight, float& r, float& g, float& b) {
		if (std::abs(weight) <= 0.00001f)
			return;
		const float cr = wheel.color.redF();
		const float cg = wheel.color.greenF();
		const float cb = wheel.color.blueF();
		const float avg = (cr + cg + cb) / 3.0f;
		const float amount = wheel.amount * weight;
		r += ((cr - avg) * amount) + (wheel.luma * weight);
		g += ((cg - avg) * amount) + (wheel.luma * weight);
		b += ((cb - avg) * amount) + (wheel.luma * weight);
	};

	unsigned char* pixels = reinterpret_cast<unsigned char*>(frame_image->bits());
	const int pixel_count = frame_image->width() * frame_image->height();

	#pragma omp parallel for if(pixel_count >= 16384) schedule(static)
	for (int pixel = 0; pixel < pixel_count; ++pixel) {
		const int idx = pixel * 4;
		const int A = pixels[idx + 3];
		if (A <= 0)
			continue;

		const float alpha_percent = static_cast<float>(A) * kInv255;
		float R = 0.0f;
		float G = 0.0f;
		float B = 0.0f;

		if (A == 255) {
			R = pixels[idx + 0] * kInv255;
			G = pixels[idx + 1] * kInv255;
			B = pixels[idx + 2] * kInv255;
		} else {
			const float inv_alpha_percent = inv_alpha[A];
			R = (pixels[idx + 0] * inv_alpha_percent) * kInv255;
			G = (pixels[idx + 1] * inv_alpha_percent) * kInv255;
			B = (pixels[idx + 2] * inv_alpha_percent) * kInv255;
		}

		const float original_r = R;
		const float original_g = G;
		const float original_b = B;

		// White-balance style controls.
		R = Clamp01(R + (temperature_value * 0.125f));
		B = Clamp01(B - (temperature_value * 0.125f));
		G = Clamp01(G - (tint_value * 0.1f));
		R = Clamp01(R + (tint_value * 0.05f));
		B = Clamp01(B + (tint_value * 0.05f));

		// Exposure and contrast.
		R = Clamp01(R * exposure_gain);
		G = Clamp01(G * exposure_gain);
		B = Clamp01(B * exposure_gain);

		R = Clamp01(((R - 0.5f) * contrast_factor) + 0.5f);
		G = Clamp01(((G - 0.5f) * contrast_factor) + 0.5f);
		B = Clamp01(((B - 0.5f) * contrast_factor) + 0.5f);

		float luma = (0.299f * R) + (0.587f * G) + (0.114f * B);
		const float shadow_weight = (1.0f - luma) * (1.0f - luma);
		const float highlight_weight = luma * luma;

		// Shadows/highlights recovery.
		const float shadow_adjust = shadows_value * shadow_weight * 0.35f;
		const float highlight_adjust = highlights_value * highlight_weight * 0.35f;
		R = Clamp01(R + shadow_adjust + highlight_adjust);
		G = Clamp01(G + shadow_adjust + highlight_adjust);
		B = Clamp01(B + shadow_adjust + highlight_adjust);

		// Tonal wheels.
		luma = (0.299f * R) + (0.587f * G) + (0.114f * B);
		float wheel_r = R;
		float wheel_g = G;
		float wheel_b = B;
		if (wheels.enabled) {
			wheel_bias(wheels.global, 1.0f, wheel_r, wheel_g, wheel_b);
			wheel_bias(wheels.shadows, (1.0f - luma) * (1.0f - luma), wheel_r, wheel_g, wheel_b);
			wheel_bias(wheels.midtones, smooth_midtones(luma), wheel_r, wheel_g, wheel_b);
			wheel_bias(wheels.highlights, luma * luma, wheel_r, wheel_g, wheel_b);
		}
		R = Clamp01(wheel_r);
		G = Clamp01(wheel_g);
		B = Clamp01(wheel_b);

		// Saturation and vibrance.
		luma = (0.299f * R) + (0.587f * G) + (0.114f * B);
		const float max_channel = std::max(R, std::max(G, B));
		const float min_channel = std::min(R, std::min(G, B));
		const float colorfulness = max_channel - min_channel;
		const float vibrance_factor = 1.0f + (vibrance_value * (1.0f - colorfulness));
		const float sat_factor = saturation_value * std::max(0.0f, vibrance_factor);
		R = Clamp01(luma + ((R - luma) * sat_factor));
		G = Clamp01(luma + ((G - luma) * sat_factor));
		B = Clamp01(luma + ((B - luma) * sat_factor));

		// Curves.
		R = sample_curve_lut(curve_master_lut, R);
		G = sample_curve_lut(curve_master_lut, G);
		B = sample_curve_lut(curve_master_lut, B);
		R = sample_curve_lut(curve_red_lut, R);
		G = sample_curve_lut(curve_green_lut, G);
		B = sample_curve_lut(curve_blue_lut, B);

		// Mix original and graded values.
		R = Clamp01((original_r * (1.0f - mix_value)) + (R * mix_value));
		G = Clamp01((original_g * (1.0f - mix_value)) + (G * mix_value));
		B = Clamp01((original_b * (1.0f - mix_value)) + (B * mix_value));

		if (A == 255) {
			pixels[idx + 0] = static_cast<unsigned char>(clampByte(R * 255.0f));
			pixels[idx + 1] = static_cast<unsigned char>(clampByte(G * 255.0f));
			pixels[idx + 2] = static_cast<unsigned char>(clampByte(B * 255.0f));
		} else {
			pixels[idx + 0] = static_cast<unsigned char>(clampByte(R * 255.0f * alpha_percent));
			pixels[idx + 1] = static_cast<unsigned char>(clampByte(G * 255.0f * alpha_percent));
			pixels[idx + 2] = static_cast<unsigned char>(clampByte(B * 255.0f * alpha_percent));
		}
	}

	if (!lut_path.empty() && lut_intensity.GetValue(frame_number) > 0.0) {
		sync_lut_effect();
		frame = lut_effect.GetFrame(frame, frame_number);
	}

	return frame;
}

std::string ColorGrade::Json() const {
	return JsonValue().toStyledString();
}

Json::Value ColorGrade::JsonValue() const {
	Json::Value root = EffectBase::JsonValue();
	root["type"] = info.class_name;
	root["temperature"] = temperature.JsonValue();
	root["tint"] = tint.JsonValue();
	root["exposure"] = exposure.JsonValue();
	root["contrast"] = contrast.JsonValue();
	root["highlights"] = highlights.JsonValue();
	root["shadows"] = shadows.JsonValue();
	root["saturation"] = saturation.JsonValue();
	root["vibrance"] = vibrance.JsonValue();
	root["mix"] = mix.JsonValue();
	root["wheels"] = wheels.JsonValue();
	root["curve_master"] = curve_master.JsonValue();
	root["curve_red"] = curve_red.JsonValue();
	root["curve_green"] = curve_green.JsonValue();
	root["curve_blue"] = curve_blue.JsonValue();
	root["lut_path"] = lut_path;
	root["lut_intensity"] = lut_intensity.JsonValue();
	return root;
}

void ColorGrade::SetJson(const std::string value) {
	try {
		const Json::Value root = openshot::stringToJson(value);
		SetJsonValue(root);
	} catch (...) {
		throw InvalidJSON("Invalid JSON for ColorGrade effect");
	}
}

void ColorGrade::SetJsonValue(const Json::Value root) {
	EffectBase::SetJsonValue(root);

	if (!root["temperature"].isNull())
		temperature.SetJsonValue(root["temperature"]);
	if (!root["tint"].isNull())
		tint.SetJsonValue(root["tint"]);
	if (!root["exposure"].isNull())
		exposure.SetJsonValue(root["exposure"]);
	if (!root["contrast"].isNull())
		contrast.SetJsonValue(root["contrast"]);
	if (!root["highlights"].isNull())
		highlights.SetJsonValue(root["highlights"]);
	if (!root["shadows"].isNull())
		shadows.SetJsonValue(root["shadows"]);
	if (!root["saturation"].isNull())
		saturation.SetJsonValue(root["saturation"]);
	if (!root["vibrance"].isNull())
		vibrance.SetJsonValue(root["vibrance"]);
	if (!root["mix"].isNull())
		mix.SetJsonValue(root["mix"]);
	if (!root["wheels"].isNull())
		wheels.SetJsonValue(root["wheels"]);
	if (!root["curve_master"].isNull())
		curve_master.SetJsonValue(root["curve_master"]);
	if (!root["curve_red"].isNull())
		curve_red.SetJsonValue(root["curve_red"]);
	if (!root["curve_green"].isNull())
		curve_green.SetJsonValue(root["curve_green"]);
	if (!root["curve_blue"].isNull())
		curve_blue.SetJsonValue(root["curve_blue"]);
	if (!root["lut_path"].isNull()) {
		lut_path = root["lut_path"].asString();
		lut_dirty = true;
	}
	if (!root["lut_intensity"].isNull()) {
		lut_intensity.SetJsonValue(root["lut_intensity"]);
		lut_dirty = true;
	}
}

std::string ColorGrade::PropertiesJSON(int64_t requested_frame) const {
	Json::Value root = BasePropertiesJSON(requested_frame);

	root["temperature"] = add_property_json("Temperature", temperature.GetValue(requested_frame), "float", "", &temperature, -1.0, 1.0, false, requested_frame);
	root["tint"] = add_property_json("Tint", tint.GetValue(requested_frame), "float", "", &tint, -1.0, 1.0, false, requested_frame);
	root["exposure"] = add_property_json("Exposure", exposure.GetValue(requested_frame), "float", "", &exposure, -2.0, 2.0, false, requested_frame);
	root["contrast"] = add_property_json("Contrast", contrast.GetValue(requested_frame), "float", "", &contrast, -1.0, 1.0, false, requested_frame);
	root["highlights"] = add_property_json("Highlights", highlights.GetValue(requested_frame), "float", "", &highlights, -1.0, 1.0, false, requested_frame);
	root["shadows"] = add_property_json("Shadows", shadows.GetValue(requested_frame), "float", "", &shadows, -1.0, 1.0, false, requested_frame);
	root["saturation"] = add_property_json("Saturation", saturation.GetValue(requested_frame), "float", "", &saturation, 0.0, 4.0, false, requested_frame);
	root["vibrance"] = add_property_json("Vibrance", vibrance.GetValue(requested_frame), "float", "", &vibrance, -1.0, 1.0, false, requested_frame);
	root["mix"] = add_property_json("Mix", mix.GetValue(requested_frame), "float", "", &mix, 0.0, 1.0, false, requested_frame);

	root["wheels"] = add_property_json("Wheels", 0.0, "colorgrade_wheels", wheels.Summary(), NULL, 0.0, 1.0, false, requested_frame);
	root["wheels"]["wheels"] = wheels.JsonValue();
	root["wheels"]["summary"] = wheels.Summary();

	root["curve_master"] = add_property_json("Curve: Master", 0.0, "colorgrade_curve", curve_master.Summary(), NULL, 0.0, 1.0, false, requested_frame);
	root["curve_master"]["curve"] = curve_master.JsonValue();
	root["curve_master"]["channel"] = "master";
	root["curve_master"]["summary"] = curve_master.Summary();

	root["curve_red"] = add_property_json("Curve: Red", 0.0, "colorgrade_curve", curve_red.Summary(), NULL, 0.0, 1.0, false, requested_frame);
	root["curve_red"]["curve"] = curve_red.JsonValue();
	root["curve_red"]["channel"] = "red";
	root["curve_red"]["summary"] = curve_red.Summary();

	root["curve_green"] = add_property_json("Curve: Green", 0.0, "colorgrade_curve", curve_green.Summary(), NULL, 0.0, 1.0, false, requested_frame);
	root["curve_green"]["curve"] = curve_green.JsonValue();
	root["curve_green"]["channel"] = "green";
	root["curve_green"]["summary"] = curve_green.Summary();

	root["curve_blue"] = add_property_json("Curve: Blue", 0.0, "colorgrade_curve", curve_blue.Summary(), NULL, 0.0, 1.0, false, requested_frame);
	root["curve_blue"]["curve"] = curve_blue.JsonValue();
	root["curve_blue"]["channel"] = "blue";
	root["curve_blue"]["summary"] = curve_blue.Summary();

	root["lut_path"] = add_property_json("LUT File", 0.0, "string", lut_path, NULL, 0, 0, false, requested_frame);
	root["lut_intensity"] = add_property_json("LUT Intensity", lut_intensity.GetValue(requested_frame), "float", "", &lut_intensity, 0.0, 1.0, false, requested_frame);

	return root.toStyledString();
}
