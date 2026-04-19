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
	 */
	class FrameScope {
	private:
		std::shared_ptr<Frame> frame;
		int waveform_columns;
		int audio_buckets;
		Json::Value scope_data;

		void reset();
		void analyze_video();
		void analyze_audio();
		void analyze();

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

		/// Return the current scope payload as a Json::Value tree.
		Json::Value JsonValue() const;

		/// Return the current scope payload as a JSON string.
		std::string Json() const;
	};

}

#endif
