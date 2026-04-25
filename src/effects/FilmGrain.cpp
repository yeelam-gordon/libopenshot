/**
 * @file
 * @brief Source file for FilmGrain effect
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "FilmGrain.h"
#include "Clip.h"
#include "Exceptions.h"
#include "Timeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

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

static float lerp(float a, float b, float t) {
	return a + ((b - a) * t);
}

static uint32_t mix32(uint32_t value) {
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

static uint32_t hash_string(const std::string& value) {
	uint32_t h = 2166136261u;
	for (unsigned char c : value) {
		h ^= c;
		h *= 16777619u;
	}
	return h;
}

static uint32_t hash_coords(uint32_t seed, int x, int y, int t, uint32_t salt) {
	uint32_t h = seed ^ salt;
	h ^= static_cast<uint32_t>(x) * 0x9e3779b9u;
	h ^= static_cast<uint32_t>(y) * 0x85ebca6bu;
	h ^= static_cast<uint32_t>(t) * 0xc2b2ae35u;
	return mix32(h);
}

static float hash_signed(uint32_t seed, int x, int y, int t, uint32_t salt) {
	return (hash_coords(seed, x, y, t, salt) / 4294967295.0f) * 2.0f - 1.0f;
}

static float value_noise(uint32_t seed, float x, float y, int time_bucket, uint32_t salt) {
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	const int x1 = x0 + 1;
	const int y1 = y0 + 1;
	const float tx = x - static_cast<float>(x0);
	const float ty = y - static_cast<float>(y0);
	const float sx = tx * tx * (3.0f - 2.0f * tx);
	const float sy = ty * ty * (3.0f - 2.0f * ty);

	const float n00 = hash_signed(seed, x0, y0, time_bucket, salt);
	const float n10 = hash_signed(seed, x1, y0, time_bucket, salt);
	const float n01 = hash_signed(seed, x0, y1, time_bucket, salt);
	const float n11 = hash_signed(seed, x1, y1, time_bucket, salt);
	const float nx0 = lerp(n00, n10, sx);
	const float nx1 = lerp(n01, n11, sx);
	return lerp(nx0, nx1, sy);
}

static float tonal_weight(float luma, float shadow_strength, float midtone_strength, float highlight_strength) {
	const float shadow = (1.0f - luma) * (1.0f - luma);
	const float highlight = luma * luma;
	const float centered = std::abs(luma - 0.5f) * 2.0f;
	const float midtone = clamp01(1.0f - centered * centered);
	return (shadow * shadow_strength) + (midtone * midtone_strength) + (highlight * highlight_strength);
}
}

FilmGrain::FilmGrain()
	: amount(0.25),
	  size(0.20),
	  softness(0.25),
	  clump(0.20),
	  shadows(0.80),
	  midtones(1.00),
	  highlights(0.55),
	  color_amount(0.20),
	  color_variation(0.35),
	  evolution(0.65),
	  coherence(0.55),
	  seed(1)
{
	init_effect_details();
}

void FilmGrain::init_effect_details() {
	InitEffectInfo();
	info.class_name = "FilmGrain";
	info.name = "Film Grain";
	info.description = "Film-inspired grain texture with tonal, color, and temporal controls.";
	info.has_audio = false;
	info.has_video = true;
}

std::shared_ptr<openshot::Frame> FilmGrain::GetFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number) {
	std::shared_ptr<QImage> frame_image = frame->GetImage();
	if (!frame_image)
		return frame;

	const float amount_value = clamp01(static_cast<float>(amount.GetValue(frame_number)));
	if (amount_value <= 0.00001f)
		return frame;

	const float size_value = clamp01(static_cast<float>(size.GetValue(frame_number)));
	const float softness_value = clamp01(static_cast<float>(softness.GetValue(frame_number)));
	const float clump_value = clamp01(static_cast<float>(clump.GetValue(frame_number)));
	const float shadows_value = clamp01(static_cast<float>(shadows.GetValue(frame_number)));
	const float midtones_value = clamp01(static_cast<float>(midtones.GetValue(frame_number)));
	const float highlights_value = clamp01(static_cast<float>(highlights.GetValue(frame_number)));
	const float color_amount_value = clamp01(static_cast<float>(color_amount.GetValue(frame_number)));
	const float color_variation_value = clamp01(static_cast<float>(color_variation.GetValue(frame_number)));
	const float evolution_value = clamp01(static_cast<float>(evolution.GetValue(frame_number)));
	const float coherence_value = clamp01(static_cast<float>(coherence.GetValue(frame_number)));

	const float cell_size = lerp(1.0f, 9.0f, size_value);
	const float fine_frequency = 1.0f / cell_size;
	const float soft_frequency = fine_frequency * lerp(0.45f, 0.12f, softness_value);
	const float clump_frequency = fine_frequency * lerp(0.35f, 0.08f, clump_value);
	const float temporal_rate = lerp(0.0f, 1.0f, evolution_value) * lerp(1.0f, 8.0f, 1.0f - coherence_value);
	const float temporal_position = static_cast<float>(frame_number) * temporal_rate;
	const int time0 = static_cast<int>(std::floor(temporal_position));
	const int time1 = time0 + 1;
	const float temporal_mix = temporal_rate <= 0.00001f ? 0.0f : clamp01(temporal_position - static_cast<float>(time0));
	const float smooth_temporal_mix = temporal_mix * temporal_mix * (3.0f - 2.0f * temporal_mix);
	const uint32_t base_seed = static_cast<uint32_t>(seed) ^ mix32(hash_string(Id()));
	const float intensity_scale = amount_value * 0.18f;
	const bool use_temporal_blend = temporal_rate > 0.00001f && smooth_temporal_mix > 0.00001f;
	const bool use_softness = softness_value > 0.00001f;
	const bool use_clump = clump_value > 0.00001f;
	const bool use_color = color_amount_value > 0.00001f;
	const bool use_color_variation = use_color && color_variation_value > 0.00001f;

	static const std::array<float, 256> inv_alpha = [] {
		std::array<float, 256> lut{};
		lut[0] = 0.0f;
		for (int i = 1; i < 256; ++i)
			lut[i] = 255.0f / static_cast<float>(i);
		return lut;
	}();

	unsigned char* pixels = reinterpret_cast<unsigned char*>(frame_image->bits());
	const int width = frame_image->width();
	const int height = frame_image->height();
	const int stride = frame_image->bytesPerLine();
	const int pixel_count = width * height;
	int reference_width = width;
	int reference_height = height;

	Clip* clip = static_cast<Clip*>(ParentClip());
	Timeline* timeline = nullptr;
	if (clip && clip->ParentTimeline())
		timeline = static_cast<Timeline*>(clip->ParentTimeline());
	else if (ParentTimeline())
		timeline = static_cast<Timeline*>(ParentTimeline());
	if (timeline && timeline->info.width > 0 && timeline->info.height > 0) {
		reference_width = timeline->info.width;
		reference_height = timeline->info.height;
	}
	const float reference_scale_x = width > 0 ? static_cast<float>(reference_width) / static_cast<float>(width) : 1.0f;
	const float reference_scale_y = height > 0 ? static_cast<float>(reference_height) / static_cast<float>(height) : 1.0f;

	#pragma omp parallel for if(pixel_count >= 16384) schedule(static)
	for (int y = 0; y < height; ++y) {
		unsigned char* row = pixels + (y * stride);
		for (int x = 0; x < width; ++x) {
			const int idx = x * 4;
			const int A = row[idx + 3];
			if (A <= 0)
				continue;

			float R = 0.0f;
			float G = 0.0f;
			float B = 0.0f;
			const float alpha_percent = static_cast<float>(A) * kInv255;
			if (A == 255) {
				R = row[idx + 0] * kInv255;
				G = row[idx + 1] * kInv255;
				B = row[idx + 2] * kInv255;
			} else {
				const float inv_alpha_percent = inv_alpha[A];
				R = (row[idx + 0] * inv_alpha_percent) * kInv255;
				G = (row[idx + 1] * inv_alpha_percent) * kInv255;
				B = (row[idx + 2] * inv_alpha_percent) * kInv255;
			}

			const float luma = (0.299f * R) + (0.587f * G) + (0.114f * B);
			const float tone = tonal_weight(luma, shadows_value, midtones_value, highlights_value);
			if (tone <= 0.00001f)
				continue;

			const float reference_x = static_cast<float>(x) * reference_scale_x;
			const float reference_y = static_cast<float>(y) * reference_scale_y;
			const float fx = reference_x * fine_frequency;
			const float fy = reference_y * fine_frequency;
			const float sx = reference_x * soft_frequency;
			const float sy = reference_y * soft_frequency;
			const float cx = reference_x * clump_frequency;
			const float cy = reference_y * clump_frequency;

			const int ix = static_cast<int>(std::floor(fx));
			const int iy = static_cast<int>(std::floor(fy));
			const auto grain_at_time = [&](uint32_t salt, int time_bucket) {
				const float fine = hash_signed(base_seed, ix, iy, time_bucket, salt);
				if (!use_softness)
					return fine;
				return lerp(fine, value_noise(base_seed, sx, sy, time_bucket, salt ^ 0x51633e2du), softness_value);
			};
			const auto grain_sample = [&](uint32_t salt) {
				float grain = grain_at_time(salt, time0);
				if (use_temporal_blend)
					grain = lerp(grain, grain_at_time(salt, time1), smooth_temporal_mix);
				if (use_clump) {
					const float cluster = value_noise(base_seed, cx, cy, time0, salt ^ 0xa511e9b3u);
					grain *= lerp(1.0f, 0.45f + (std::abs(cluster) * 1.35f), clump_value);
				}
				return grain;
			};

			const float luma_grain = grain_sample(0x1000193u);
			float red_grain = luma_grain;
			float green_grain = luma_grain;
			float blue_grain = luma_grain;
			if (use_color_variation) {
				red_grain = lerp(luma_grain, grain_sample(0x8da6b343u), color_variation_value);
				green_grain = lerp(luma_grain, grain_sample(0xd8163841u), color_variation_value);
				blue_grain = lerp(luma_grain, grain_sample(0xcb1ab31fu), color_variation_value);
			}
			const float strength = intensity_scale * tone;

			if (use_color) {
				R = clamp01(R + (lerp(luma_grain, red_grain, color_amount_value) * strength));
				G = clamp01(G + (lerp(luma_grain, green_grain, color_amount_value) * strength));
				B = clamp01(B + (lerp(luma_grain, blue_grain, color_amount_value) * strength));
			} else {
				const float delta = luma_grain * strength;
				R = clamp01(R + delta);
				G = clamp01(G + delta);
				B = clamp01(B + delta);
			}

			if (A == 255) {
				row[idx + 0] = static_cast<unsigned char>(clampByte(R * 255.0f));
				row[idx + 1] = static_cast<unsigned char>(clampByte(G * 255.0f));
				row[idx + 2] = static_cast<unsigned char>(clampByte(B * 255.0f));
			} else {
				row[idx + 0] = static_cast<unsigned char>(clampByte(R * 255.0f * alpha_percent));
				row[idx + 1] = static_cast<unsigned char>(clampByte(G * 255.0f * alpha_percent));
				row[idx + 2] = static_cast<unsigned char>(clampByte(B * 255.0f * alpha_percent));
			}
		}
	}

	return frame;
}

std::string FilmGrain::Json() const {
	return JsonValue().toStyledString();
}

Json::Value FilmGrain::JsonValue() const {
	Json::Value root = EffectBase::JsonValue();
	root["type"] = info.class_name;
	root["amount"] = amount.JsonValue();
	root["size"] = size.JsonValue();
	root["softness"] = softness.JsonValue();
	root["clump"] = clump.JsonValue();
	root["shadows"] = shadows.JsonValue();
	root["midtones"] = midtones.JsonValue();
	root["highlights"] = highlights.JsonValue();
	root["color_amount"] = color_amount.JsonValue();
	root["color_variation"] = color_variation.JsonValue();
	root["evolution"] = evolution.JsonValue();
	root["coherence"] = coherence.JsonValue();
	root["seed"] = seed;
	return root;
}

void FilmGrain::SetJson(const std::string value) {
	try {
		const Json::Value root = openshot::stringToJson(value);
		SetJsonValue(root);
	} catch (const std::exception&) {
		throw InvalidJSON("Invalid JSON for FilmGrain effect");
	}
}

void FilmGrain::SetJsonValue(const Json::Value root) {
	EffectBase::SetJsonValue(root);
	if (!root["amount"].isNull())
		amount.SetJsonValue(root["amount"]);
	if (!root["size"].isNull())
		size.SetJsonValue(root["size"]);
	if (!root["softness"].isNull())
		softness.SetJsonValue(root["softness"]);
	if (!root["clump"].isNull())
		clump.SetJsonValue(root["clump"]);
	if (!root["shadows"].isNull())
		shadows.SetJsonValue(root["shadows"]);
	if (!root["midtones"].isNull())
		midtones.SetJsonValue(root["midtones"]);
	if (!root["highlights"].isNull())
		highlights.SetJsonValue(root["highlights"]);
	if (!root["color_amount"].isNull())
		color_amount.SetJsonValue(root["color_amount"]);
	if (!root["color_variation"].isNull())
		color_variation.SetJsonValue(root["color_variation"]);
	if (!root["evolution"].isNull())
		evolution.SetJsonValue(root["evolution"]);
	if (!root["coherence"].isNull())
		coherence.SetJsonValue(root["coherence"]);
	if (!root["seed"].isNull())
		seed = root["seed"].asInt();
}

std::string FilmGrain::PropertiesJSON(int64_t requested_frame) const {
	Json::Value root = BasePropertiesJSON(requested_frame);
	root["amount"] = add_property_json("Amount", amount.GetValue(requested_frame), "float", "Overall grain intensity.", &amount, 0.0, 1.0, false, requested_frame);
	root["size"] = add_property_json("Size", size.GetValue(requested_frame), "float", "Fine to coarse grain scale.", &size, 0.0, 1.0, false, requested_frame);
	root["softness"] = add_property_json("Softness", softness.GetValue(requested_frame), "float", "Hard crisp grain to softer organic grain.", &softness, 0.0, 1.0, false, requested_frame);
	root["clump"] = add_property_json("Clump", clump.GetValue(requested_frame), "float", "Even grain to clustered irregular grain.", &clump, 0.0, 1.0, false, requested_frame);
	root["shadows"] = add_property_json("Shadows", shadows.GetValue(requested_frame), "float", "Grain strength in dark regions.", &shadows, 0.0, 1.0, false, requested_frame);
	root["midtones"] = add_property_json("Midtones", midtones.GetValue(requested_frame), "float", "Grain strength in middle tonal regions.", &midtones, 0.0, 1.0, false, requested_frame);
	root["highlights"] = add_property_json("Highlights", highlights.GetValue(requested_frame), "float", "Grain strength in bright regions.", &highlights, 0.0, 1.0, false, requested_frame);
	root["color_amount"] = add_property_json("Color Amount", color_amount.GetValue(requested_frame), "float", "How much grain affects chroma instead of mostly luma.", &color_amount, 0.0, 1.0, false, requested_frame);
	root["color_variation"] = add_property_json("Color Variation", color_variation.GetValue(requested_frame), "float", "Correlated to independently varied color grain.", &color_variation, 0.0, 1.0, false, requested_frame);
	root["evolution"] = add_property_json("Evolution", evolution.GetValue(requested_frame), "float", "How much grain renews over time.", &evolution, 0.0, 1.0, false, requested_frame);
	root["coherence"] = add_property_json("Coherence", coherence.GetValue(requested_frame), "float", "How stable and smooth grain remains between frames.", &coherence, 0.0, 1.0, false, requested_frame);
	root["seed"] = add_property_json("Seed", seed, "int", "Deterministic grain variation.", NULL, 0, 1000000, false, requested_frame);
	return root.toStyledString();
}
