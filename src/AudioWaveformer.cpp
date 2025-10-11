/**
 * @file
 * @brief Source file for AudioWaveformer class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2022 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "AudioWaveformer.h"

#include <cmath>

#include <algorithm>
#include <vector>

#include "Clip.h"


using namespace std;
using namespace openshot;


// Default constructor
AudioWaveformer::AudioWaveformer(ReaderBase* new_reader) : reader(new_reader)
{

}

// Destructor
AudioWaveformer::~AudioWaveformer()
{

}

// Extract audio samples from any ReaderBase class
AudioWaveformData AudioWaveformer::ExtractSamples(int channel, int num_per_second, bool normalize) {
	AudioWaveformData data;

	if (!reader || num_per_second <= 0) {
		return data;
	}

	// Open reader (if needed)
	bool does_reader_have_video = reader->info.has_video;
	if (!reader->IsOpen()) {
		reader->Open();
	}
	// Disable video for faster processing
	reader->info.has_video = false;

	int sample_rate = reader->info.sample_rate;
	if (sample_rate <= 0) {
		sample_rate = num_per_second;
	}
	int sample_divisor = sample_rate / num_per_second;
	if (sample_divisor <= 0) {
		sample_divisor = 1;
	}

	// Determine length of video frames (for waveform)
	int64_t reader_video_length = reader->info.video_length;
	if (const auto *clip = dynamic_cast<Clip*>(reader)) {
		// If Clip-based reader, and time keyframes present
		if (clip->time.GetCount() > 1) {
			reader_video_length = clip->time.GetLength();
		}
	}
	if (reader_video_length < 0) {
		reader_video_length = 0;
	}
	float reader_duration = reader->info.duration;
	double fps_value = reader->info.fps.ToDouble();
	float frames_duration = 0.0f;
	if (reader_video_length > 0 && fps_value > 0.0) {
		frames_duration = static_cast<float>(reader_video_length / fps_value);
	}
	const bool has_source_length = reader->info.video_length > 0;
	const bool frames_extended = has_source_length && reader_video_length > reader->info.video_length;
	if (reader_duration <= 0.0f) {
		reader_duration = frames_duration;
	} else if ((frames_extended || !has_source_length) && frames_duration > reader_duration + 1e-4f) {
		reader_duration = frames_duration;
	}
	if (reader_duration < 0.0f) {
		reader_duration = 0.0f;
	}

	if (!reader->info.has_audio) {
		reader->info.has_video = does_reader_have_video;
		return data;
	}

	int total_samples = static_cast<int>(std::ceil(reader_duration * num_per_second));
	if (total_samples <= 0 || reader->info.channels == 0) {
		reader->info.has_video = does_reader_have_video;
		return data;
	}

	if (channel != -1 && (channel < 0 || channel >= reader->info.channels)) {
		reader->info.has_video = does_reader_have_video;
		return data;
	}

	// Resize and clear audio buffers
	data.resize(total_samples);
	data.zero(total_samples);

	int extracted_index = 0;
	int sample_index = 0;
	float samples_max = 0.0f;
	float chunk_max = 0.0f;
	float chunk_squared_sum = 0.0f;

	int channel_count = (channel == -1) ? reader->info.channels : 1;
	std::vector<float*> channels(reader->info.channels, nullptr);

	for (int64_t f = 1; f <= reader_video_length && extracted_index < total_samples; f++) {
		std::shared_ptr<openshot::Frame> frame = reader->GetFrame(f);

		for (int channel_index = 0; channel_index < reader->info.channels; channel_index++) {
			if (channel == channel_index || channel == -1) {
				channels[channel_index] = frame->GetAudioSamples(channel_index);
			}
		}

		for (int s = 0; s < frame->GetAudioSamplesCount(); s++) {
			for (int channel_index = 0; channel_index < reader->info.channels; channel_index++) {
				if (channel == channel_index || channel == -1) {
					float *samples = channels[channel_index];
					if (!samples) {
						continue;
					}
					float rms_sample_value = std::sqrt(samples[s] * samples[s]);

					chunk_squared_sum += rms_sample_value;
					chunk_max = std::max(chunk_max, rms_sample_value);
				}
			}

			sample_index += 1;

			if (sample_index % sample_divisor == 0) {
				float avg_squared_sum = 0.0f;
				if (channel_count > 0) {
					avg_squared_sum = chunk_squared_sum / static_cast<float>(sample_divisor * channel_count);
				}

				if (extracted_index < total_samples) {
					data.max_samples[extracted_index] = chunk_max;
					data.rms_samples[extracted_index] = avg_squared_sum;
					samples_max = std::max(samples_max, chunk_max);
					extracted_index++;
				}

				sample_index = 0;
				chunk_max = 0.0f;
				chunk_squared_sum = 0.0f;

				if (extracted_index >= total_samples) {
					break;
				}
			}
		}
	}

	if (sample_index > 0 && extracted_index < total_samples) {
		float avg_squared_sum = 0.0f;
		if (channel_count > 0) {
			avg_squared_sum = chunk_squared_sum / static_cast<float>(sample_index * channel_count);
		}

		data.max_samples[extracted_index] = chunk_max;
		data.rms_samples[extracted_index] = avg_squared_sum;
		samples_max = std::max(samples_max, chunk_max);
		extracted_index++;
	}

	if (normalize && samples_max > 0.0f) {
		float scale = 1.0f / samples_max;
		data.scale(total_samples, scale);
	}

	reader->info.has_video = does_reader_have_video;

	return data;
}

