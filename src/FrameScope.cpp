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

static Json::Value json_array_from_vector(const std::vector<int>& values) {
	Json::Value array(Json::arrayValue);
	for (size_t i = 0; i < values.size(); ++i)
		array.append(values[i]);
	return array;
}

static Json::Value json_array_from_vector(const std::vector<float>& values) {
	Json::Value array(Json::arrayValue);
	for (size_t i = 0; i < values.size(); ++i)
		array.append(values[i]);
	return array;
}
}

FrameScope::FrameScope()
	: frame(nullptr), waveform_columns(256), audio_buckets(256), waveform_bins(256), json_dirty(true) {
	reset();
}

FrameScope::FrameScope(std::shared_ptr<Frame> new_frame, int new_waveform_columns, int new_audio_buckets)
	: frame(new_frame),
	  waveform_columns(std::max(1, new_waveform_columns)),
	  audio_buckets(std::max(1, new_audio_buckets)),
	  waveform_bins(256),
	  json_dirty(true) {
	analyze();
}

void FrameScope::reset() {
	video_present = false;
	video_width = 0;
	video_height = 0;
	avg_luma = 0.0;
	clipped_shadows = 0;
	clipped_highlights = 0;
	clipped_red = 0;
	clipped_green = 0;
	clipped_blue = 0;
	histogram_luma.assign(256, 0);
	histogram_red.assign(256, 0);
	histogram_green.assign(256, 0);
	histogram_blue.assign(256, 0);
	waveform_luma.assign(static_cast<size_t>(waveform_columns) * waveform_bins, 0);
	waveform_red.assign(static_cast<size_t>(waveform_columns) * waveform_bins, 0);
	waveform_green.assign(static_cast<size_t>(waveform_columns) * waveform_bins, 0);
	waveform_blue.assign(static_cast<size_t>(waveform_columns) * waveform_bins, 0);

	audio_present = false;
	audio_channels = 0;
	audio_samples = 0;
	audio_sample_rate = 0;
	audio_peak.clear();
	audio_rms.clear();
	audio_clipped_samples.clear();
	audio_waveform_min.clear();
	audio_waveform_max.clear();
	json_dirty = true;
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
	json_dirty = true;
}

void FrameScope::analyze_video() {
	std::shared_ptr<QImage> image = frame->GetImage();
	if (!image || image->isNull())
		return;

	video_present = true;
	video_width = image->width();
	video_height = image->height();

	std::fill(histogram_luma.begin(), histogram_luma.end(), 0);
	std::fill(histogram_red.begin(), histogram_red.end(), 0);
	std::fill(histogram_green.begin(), histogram_green.end(), 0);
	std::fill(histogram_blue.begin(), histogram_blue.end(), 0);
	std::fill(waveform_luma.begin(), waveform_luma.end(), 0);
	std::fill(waveform_red.begin(), waveform_red.end(), 0);
	std::fill(waveform_green.begin(), waveform_green.end(), 0);
	std::fill(waveform_blue.begin(), waveform_blue.end(), 0);

	double luma_sum = 0.0;
	clipped_shadows = 0;
	clipped_highlights = 0;
	clipped_red = 0;
	clipped_green = 0;
	clipped_blue = 0;

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

				histogram_luma[luma_idx]++;
				histogram_red[red_idx]++;
				histogram_green[green_idx]++;
				histogram_blue[blue_idx]++;
				const size_t waveform_offset = static_cast<size_t>(column) * static_cast<size_t>(waveform_bins);
				waveform_luma[waveform_offset + luma_idx]++;
				waveform_red[waveform_offset + red_idx]++;
				waveform_green[waveform_offset + green_idx]++;
				waveform_blue[waveform_offset + blue_idx]++;

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
	avg_luma = pixel_total > 0.0 ? (luma_sum / pixel_total) : 0.0;
}

void FrameScope::analyze_audio() {
	if (!frame->has_audio_data || !frame->audio)
		return;

	const int channels = frame->GetAudioChannelsCount();
	const int samples = frame->GetAudioSamplesCount();
	if (channels <= 0 || samples <= 0)
		return;

	audio_present = true;
	audio_channels = channels;
	audio_samples = samples;
	audio_sample_rate = frame->SampleRate();

	audio_peak.assign(static_cast<size_t>(channels), 0.0f);
	std::vector<double> rms_sums(static_cast<size_t>(channels), 0.0);
	audio_rms.assign(static_cast<size_t>(channels), 0.0f);
	audio_clipped_samples.assign(static_cast<size_t>(channels), 0);
	audio_waveform_min.assign(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(audio_buckets), 0.0f));
	audio_waveform_max.assign(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(audio_buckets), 0.0f));

	for (int channel = 0; channel < channels; ++channel) {
		float* channel_samples = frame->GetAudioSamples(channel);
		if (!channel_samples)
			continue;

		std::fill(audio_waveform_min[channel].begin(), audio_waveform_min[channel].end(), 1.0f);
		std::fill(audio_waveform_max[channel].begin(), audio_waveform_max[channel].end(), -1.0f);

		for (int sample = 0; sample < samples; ++sample) {
			const float value = channel_samples[sample];
			const float abs_value = std::abs(value);
			const int bucket = clamp_int((sample * audio_buckets) / std::max(1, samples), 0, audio_buckets - 1);

			audio_peak[channel] = std::max(audio_peak[channel], abs_value);
			rms_sums[channel] += static_cast<double>(value) * static_cast<double>(value);
			if (abs_value >= 0.999f)
				audio_clipped_samples[channel]++;

			audio_waveform_min[channel][bucket] = std::min(audio_waveform_min[channel][bucket], value);
			audio_waveform_max[channel][bucket] = std::max(audio_waveform_max[channel][bucket], value);
		}

		for (int bucket = 0; bucket < audio_buckets; ++bucket) {
			if (audio_waveform_min[channel][bucket] > audio_waveform_max[channel][bucket]) {
				audio_waveform_min[channel][bucket] = 0.0f;
				audio_waveform_max[channel][bucket] = 0.0f;
			}
		}
	}

	for (int channel = 0; channel < channels; ++channel) {
		audio_rms[channel] = samples > 0 ? static_cast<float>(std::sqrt(rms_sums[channel] / static_cast<double>(samples))) : 0.0f;
	}
}

void FrameScope::rebuild_json() const {
	scope_data = Json::Value(Json::objectValue);
	scope_data["version"] = 1;

	Json::Value video(Json::objectValue);
	video["present"] = video_present;
	if (video_present) {
		video["width"] = video_width;
		video["height"] = video_height;

		video["summary"] = Json::Value(Json::objectValue);
		video["summary"]["avg_luma"] = avg_luma;
		video["summary"]["clipped_shadows"] = clipped_shadows;
		video["summary"]["clipped_highlights"] = clipped_highlights;
		video["summary"]["clipped_red"] = clipped_red;
		video["summary"]["clipped_green"] = clipped_green;
		video["summary"]["clipped_blue"] = clipped_blue;

		video["histogram"] = Json::Value(Json::objectValue);
		video["histogram"]["luma"] = json_array_from_vector(histogram_luma);
		video["histogram"]["red"] = json_array_from_vector(histogram_red);
		video["histogram"]["green"] = json_array_from_vector(histogram_green);
		video["histogram"]["blue"] = json_array_from_vector(histogram_blue);

		video["waveform"] = Json::Value(Json::objectValue);
		video["waveform"]["columns"] = waveform_columns;
		video["waveform"]["bins"] = waveform_bins;
		video["waveform"]["luma"] = json_array_from_vector(waveform_luma);
		video["waveform"]["red"] = json_array_from_vector(waveform_red);
		video["waveform"]["green"] = json_array_from_vector(waveform_green);
		video["waveform"]["blue"] = json_array_from_vector(waveform_blue);
	}
	scope_data["video"] = video;

	Json::Value audio(Json::objectValue);
	audio["present"] = audio_present;
	if (audio_present) {
		audio["channels"] = audio_channels;
		audio["samples"] = audio_samples;
		audio["sample_rate"] = audio_sample_rate;

		audio["summary"] = Json::Value(Json::objectValue);
		audio["summary"]["peak"] = json_array_from_vector(audio_peak);
		audio["summary"]["rms"] = json_array_from_vector(audio_rms);
		audio["summary"]["clipped_samples"] = json_array_from_vector(audio_clipped_samples);

		audio["waveform"] = Json::Value(Json::objectValue);
		audio["waveform"]["buckets"] = audio_buckets;
		audio["waveform"]["min"] = Json::Value(Json::arrayValue);
		audio["waveform"]["max"] = Json::Value(Json::arrayValue);
		for (int channel = 0; channel < audio_channels; ++channel) {
			audio["waveform"]["min"].append(json_array_from_vector(audio_waveform_min[static_cast<size_t>(channel)]));
			audio["waveform"]["max"].append(json_array_from_vector(audio_waveform_max[static_cast<size_t>(channel)]));
		}
	}
	scope_data["audio"] = audio;
	json_dirty = false;
}

Json::Value FrameScope::JsonValue() const {
	if (json_dirty)
		rebuild_json();
	return scope_data;
}

std::string FrameScope::Json() const {
	if (json_dirty)
		rebuild_json();
	return scope_data.toStyledString();
}

std::vector<float> FrameScope::GetAudioWaveformMin(int channel) const {
	if (channel < 0 || channel >= static_cast<int>(audio_waveform_min.size()))
		return std::vector<float>();
	return audio_waveform_min[static_cast<size_t>(channel)];
}

std::vector<float> FrameScope::GetAudioWaveformMax(int channel) const {
	if (channel < 0 || channel >= static_cast<int>(audio_waveform_max.size()))
		return std::vector<float>();
	return audio_waveform_max[static_cast<size_t>(channel)];
}
