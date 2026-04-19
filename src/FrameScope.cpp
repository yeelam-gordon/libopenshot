/**
 * @file
 * @brief Source file for FrameScope class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "FrameScope.h"

#include <algorithm>
#include <cmath>

using namespace openshot;

namespace {
constexpr float kInv255 = 1.0f / 255.0f;

static int clamp_int(int value, int min_value, int max_value) {
	return std::max(min_value, std::min(max_value, value));
}

static int byte_bin(float value) {
	return clamp_int(static_cast<int>(std::round(value * 255.0f)), 0, 255);
}
}

FrameScope::FrameScope()
	: frame(nullptr), waveform_columns(256), audio_buckets(256) {
	reset();
}

FrameScope::FrameScope(std::shared_ptr<Frame> new_frame, int new_waveform_columns, int new_audio_buckets)
	: frame(new_frame),
	  waveform_columns(std::max(1, new_waveform_columns)),
	  audio_buckets(std::max(1, new_audio_buckets)) {
	analyze();
}

void FrameScope::reset() {
	scope_data = Json::Value(Json::objectValue);
	scope_data["version"] = 1;
	scope_data["video"] = Json::Value(Json::objectValue);
	scope_data["audio"] = Json::Value(Json::objectValue);
	scope_data["video"]["present"] = false;
	scope_data["audio"]["present"] = false;
}

void FrameScope::SetFrame(std::shared_ptr<Frame> new_frame) {
	frame = new_frame;
	analyze();
}

void FrameScope::SetWaveformColumns(int columns) {
	waveform_columns = std::max(1, columns);
	analyze();
}

void FrameScope::SetAudioBuckets(int buckets) {
	audio_buckets = std::max(1, buckets);
	analyze();
}

void FrameScope::analyze() {
	reset();
	if (!frame)
		return;

	analyze_video();
	analyze_audio();
}

void FrameScope::analyze_video() {
	std::shared_ptr<QImage> image = frame->GetImage();
	if (!image || image->isNull())
		return;

	Json::Value video(Json::objectValue);
	video["present"] = true;
	video["width"] = image->width();
	video["height"] = image->height();

	std::vector<int> hist_luma(256, 0);
	std::vector<int> hist_red(256, 0);
	std::vector<int> hist_green(256, 0);
	std::vector<int> hist_blue(256, 0);
	std::vector<int> waveform(static_cast<size_t>(waveform_columns) * 256, 0);

	double luma_sum = 0.0;
	int clipped_shadows = 0;
	int clipped_highlights = 0;
	int clipped_red = 0;
	int clipped_green = 0;
	int clipped_blue = 0;

	const int width = image->width();
	const int height = image->height();
	const int bytes_per_line = image->bytesPerLine();
	const unsigned char* bits = image->constBits();

	for (int y = 0; y < height; ++y) {
		const unsigned char* row = bits + (static_cast<size_t>(y) * bytes_per_line);
		for (int x = 0; x < width; ++x) {
			const unsigned char* pixel = row + (static_cast<size_t>(x) * 4);
			const int blue = pixel[0];
			const int green = pixel[1];
			const int red = pixel[2];
			const int alpha = pixel[3];
			if (alpha <= 0)
				continue;

			const float alpha_percent = static_cast<float>(alpha) * kInv255;
			const float inv_alpha = alpha == 255 ? 1.0f : (1.0f / alpha_percent);
			const float redf = std::min(1.0f, (red * kInv255) * inv_alpha);
			const float greenf = std::min(1.0f, (green * kInv255) * inv_alpha);
			const float bluef = std::min(1.0f, (blue * kInv255) * inv_alpha);
			const float luma = (0.299f * redf) + (0.587f * greenf) + (0.114f * bluef);

			const int luma_idx = byte_bin(luma);
			const int red_idx = byte_bin(redf);
			const int green_idx = byte_bin(greenf);
			const int blue_idx = byte_bin(bluef);
			const int column = clamp_int((x * waveform_columns) / std::max(1, width), 0, waveform_columns - 1);

			hist_luma[luma_idx]++;
			hist_red[red_idx]++;
			hist_green[green_idx]++;
			hist_blue[blue_idx]++;
			waveform[static_cast<size_t>(column) * 256 + luma_idx]++;

			luma_sum += luma;
			if (luma_idx <= 2)
				clipped_shadows++;
			if (luma_idx >= 253)
				clipped_highlights++;
			if (red_idx >= 253)
				clipped_red++;
			if (green_idx >= 253)
				clipped_green++;
			if (blue_idx >= 253)
				clipped_blue++;
		}
	}

	const double pixel_total = static_cast<double>(width) * static_cast<double>(height);
	video["summary"] = Json::Value(Json::objectValue);
	video["summary"]["avg_luma"] = pixel_total > 0.0 ? (luma_sum / pixel_total) : 0.0;
	video["summary"]["clipped_shadows"] = clipped_shadows;
	video["summary"]["clipped_highlights"] = clipped_highlights;
	video["summary"]["clipped_red"] = clipped_red;
	video["summary"]["clipped_green"] = clipped_green;
	video["summary"]["clipped_blue"] = clipped_blue;

	video["histogram"] = Json::Value(Json::objectValue);
	video["histogram"]["luma"] = Json::Value(Json::arrayValue);
	video["histogram"]["red"] = Json::Value(Json::arrayValue);
	video["histogram"]["green"] = Json::Value(Json::arrayValue);
	video["histogram"]["blue"] = Json::Value(Json::arrayValue);
	for (int i = 0; i < 256; ++i) {
		video["histogram"]["luma"].append(hist_luma[i]);
		video["histogram"]["red"].append(hist_red[i]);
		video["histogram"]["green"].append(hist_green[i]);
		video["histogram"]["blue"].append(hist_blue[i]);
	}

	video["waveform"] = Json::Value(Json::objectValue);
	video["waveform"]["columns"] = waveform_columns;
	video["waveform"]["bins"] = 256;
	video["waveform"]["luma"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < waveform.size(); ++i) {
		video["waveform"]["luma"].append(waveform[i]);
	}

	scope_data["video"] = video;
}

void FrameScope::analyze_audio() {
	if (!frame->has_audio_data || !frame->audio)
		return;

	const int channels = frame->GetAudioChannelsCount();
	const int samples = frame->GetAudioSamplesCount();
	if (channels <= 0 || samples <= 0)
		return;

	Json::Value audio(Json::objectValue);
	audio["present"] = true;
	audio["channels"] = channels;
	audio["samples"] = samples;
	audio["sample_rate"] = frame->SampleRate();

	std::vector<float> peak_values(static_cast<size_t>(channels), 0.0f);
	std::vector<double> rms_sums(static_cast<size_t>(channels), 0.0);
	std::vector<int> clipped_samples(static_cast<size_t>(channels), 0);
	std::vector<std::vector<float>> min_buckets(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(audio_buckets), 0.0f));
	std::vector<std::vector<float>> max_buckets(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(audio_buckets), 0.0f));

	for (int channel = 0; channel < channels; ++channel) {
		float* channel_samples = frame->GetAudioSamples(channel);
		if (!channel_samples)
			continue;

		std::fill(min_buckets[channel].begin(), min_buckets[channel].end(), 1.0f);
		std::fill(max_buckets[channel].begin(), max_buckets[channel].end(), -1.0f);

		for (int sample = 0; sample < samples; ++sample) {
			const float value = channel_samples[sample];
			const float abs_value = std::abs(value);
			const int bucket = clamp_int((sample * audio_buckets) / std::max(1, samples), 0, audio_buckets - 1);

			peak_values[channel] = std::max(peak_values[channel], abs_value);
			rms_sums[channel] += static_cast<double>(value) * static_cast<double>(value);
			if (abs_value >= 0.999f)
				clipped_samples[channel]++;

			min_buckets[channel][bucket] = std::min(min_buckets[channel][bucket], value);
			max_buckets[channel][bucket] = std::max(max_buckets[channel][bucket], value);
		}

		for (int bucket = 0; bucket < audio_buckets; ++bucket) {
			if (min_buckets[channel][bucket] > max_buckets[channel][bucket]) {
				min_buckets[channel][bucket] = 0.0f;
				max_buckets[channel][bucket] = 0.0f;
			}
		}
	}

	audio["summary"] = Json::Value(Json::objectValue);
	audio["summary"]["peak"] = Json::Value(Json::arrayValue);
	audio["summary"]["rms"] = Json::Value(Json::arrayValue);
	audio["summary"]["clipped_samples"] = Json::Value(Json::arrayValue);
	for (int channel = 0; channel < channels; ++channel) {
		audio["summary"]["peak"].append(peak_values[channel]);
		audio["summary"]["rms"].append(samples > 0 ? std::sqrt(rms_sums[channel] / static_cast<double>(samples)) : 0.0);
		audio["summary"]["clipped_samples"].append(clipped_samples[channel]);
	}

	audio["waveform"] = Json::Value(Json::objectValue);
	audio["waveform"]["buckets"] = audio_buckets;
	audio["waveform"]["min"] = Json::Value(Json::arrayValue);
	audio["waveform"]["max"] = Json::Value(Json::arrayValue);
	for (int channel = 0; channel < channels; ++channel) {
		Json::Value min_values(Json::arrayValue);
		Json::Value max_values(Json::arrayValue);
		for (int bucket = 0; bucket < audio_buckets; ++bucket) {
			min_values.append(min_buckets[channel][bucket]);
			max_values.append(max_buckets[channel][bucket]);
		}
		audio["waveform"]["min"].append(min_values);
		audio["waveform"]["max"].append(max_values);
	}

	scope_data["audio"] = audio;
}

Json::Value FrameScope::JsonValue() const {
	return scope_data;
}

std::string FrameScope::Json() const {
	return scope_data.toStyledString();
}
