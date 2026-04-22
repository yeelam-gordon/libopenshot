/**
 * @file
 * @brief Header file for FrameScope class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef OPENSHOT_FRAMESCOPE_H
#define OPENSHOT_FRAMESCOPE_H

#include "Frame.h"
#include "Json.h"

#include <memory>
#include <string>
#include <vector>

namespace openshot {

	/**
	 * @brief Analyze a single Frame and expose scope-friendly JSON.
	 *
	 * FrameScope provides a lightweight analysis layer for the current preview
	 * frame. It intentionally focuses on broadly useful data for grading and
	 * editorial decisions, such as histograms, a luma waveform, audio envelope
	 * buckets, and simple clipping / peak summaries.
	 *
	 * Pixel format: libopenshot frames are always stored as
	 * QImage::Format_RGBA8888_Premultiplied (see Frame::AddImage). The
	 * in-memory byte order per pixel is [R=0, G=1, B=2, A=3].
	 */
	class FrameScope {
	private:
		std::shared_ptr<Frame> frame;
		int waveform_columns;
		int audio_buckets;

		bool video_present;
		int video_width;
		int video_height;
		int waveform_bins;
		double avg_luma;
		int clipped_shadows;
		int clipped_highlights;
		int clipped_red;
		int clipped_green;
		int clipped_blue;
		std::vector<int> histogram_luma;
		std::vector<int> histogram_red;
		std::vector<int> histogram_green;
		std::vector<int> histogram_blue;
		std::vector<int> waveform_luma;
		std::vector<int> waveform_red;
		std::vector<int> waveform_green;
		std::vector<int> waveform_blue;

		bool audio_present;
		int audio_channels;
		int audio_samples;
		int audio_sample_rate;
		std::vector<float> audio_peak;
		std::vector<float> audio_rms;
		std::vector<int> audio_clipped_samples;
		std::vector<std::vector<float>> audio_waveform_min;
		std::vector<std::vector<float>> audio_waveform_max;
		mutable Json::Value scope_data;
		mutable bool json_dirty;

		void reset();
		void analyze_video();
		void analyze_audio();
		void analyze();
		void rebuild_json() const;

	public:
		/// Create an empty scope analyzer with default bucket sizes.
		FrameScope();

		/// Construct and immediately analyze a frame.
		FrameScope(std::shared_ptr<Frame> frame, int waveform_columns = 256, int audio_buckets = 256);

		/// Replace the current frame and recompute the scope data.
		void SetFrame(std::shared_ptr<Frame> new_frame);

		/// Return the currently analyzed frame.
		std::shared_ptr<Frame> GetFrame() const { return frame; }

		/// Set the number of horizontal waveform columns and re-analyze.
		void SetWaveformColumns(int columns);

		/// Set the number of audio buckets and re-analyze.
		void SetAudioBuckets(int buckets);

		/// Return whether the current frame has analyzable video data.
		bool HasVideo() const { return video_present; }

		/// Return whether the current frame has analyzable audio data.
		bool HasAudio() const { return audio_present; }

		/// Return the analyzed video width.
		int GetVideoWidth() const { return video_width; }

		/// Return the analyzed video height.
		int GetVideoHeight() const { return video_height; }

		/// Return the number of waveform columns.
		int GetWaveformColumns() const { return waveform_columns; }

		/// Return the number of vertical waveform bins.
		int GetWaveformBins() const { return waveform_bins; }

		/// Return the luma histogram bins.
		std::vector<int> GetVideoHistogramLuma() const { return histogram_luma; }

		/// Return the red histogram bins.
		std::vector<int> GetVideoHistogramRed() const { return histogram_red; }

		/// Return the green histogram bins.
		std::vector<int> GetVideoHistogramGreen() const { return histogram_green; }

		/// Return the blue histogram bins.
		std::vector<int> GetVideoHistogramBlue() const { return histogram_blue; }

		/// Return the flattened luma waveform bins.
		std::vector<int> GetVideoWaveformLuma() const { return waveform_luma; }

		/// Return the flattened red waveform bins.
		std::vector<int> GetVideoWaveformRed() const { return waveform_red; }

		/// Return the flattened green waveform bins.
		std::vector<int> GetVideoWaveformGreen() const { return waveform_green; }

		/// Return the flattened blue waveform bins.
		std::vector<int> GetVideoWaveformBlue() const { return waveform_blue; }

		/// Return the average luma of the analyzed frame.
		double GetVideoAverageLuma() const { return avg_luma; }

		/// Return the clipped shadow pixel count.
		int GetVideoClippedShadows() const { return clipped_shadows; }

		/// Return the clipped highlight pixel count.
		int GetVideoClippedHighlights() const { return clipped_highlights; }

		/// Return the clipped red-channel pixel count.
		int GetVideoClippedRed() const { return clipped_red; }

		/// Return the clipped green-channel pixel count.
		int GetVideoClippedGreen() const { return clipped_green; }

		/// Return the clipped blue-channel pixel count.
		int GetVideoClippedBlue() const { return clipped_blue; }

		/// Return the number of analyzed audio channels.
		int GetAudioChannels() const { return audio_channels; }

		/// Return the number of analyzed audio samples.
		int GetAudioSamples() const { return audio_samples; }

		/// Return the analyzed audio sample rate.
		int GetAudioSampleRate() const { return audio_sample_rate; }

		/// Return the number of audio waveform buckets.
		int GetAudioBuckets() const { return audio_buckets; }

		/// Return per-channel peak levels.
		std::vector<float> GetAudioPeakLevels() const { return audio_peak; }

		/// Return per-channel RMS levels.
		std::vector<float> GetAudioRmsLevels() const { return audio_rms; }

		/// Return per-channel clipped sample counts.
		std::vector<int> GetAudioClippedSamples() const { return audio_clipped_samples; }

		/// Return one channel of audio waveform minimum values.
		std::vector<float> GetAudioWaveformMin(int channel) const;

		/// Return one channel of audio waveform maximum values.
		std::vector<float> GetAudioWaveformMax(int channel) const;

		/// Return the current scope payload as a Json::Value tree.
		Json::Value JsonValue() const;

		/// Return the current scope payload as a JSON string.
		std::string Json() const;
	};

}

#endif
