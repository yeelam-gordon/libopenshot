/**
 * @file
 * @brief Benchmark executable for core libopenshot operations
 * @author Jonathan Thomas <jonathan@openshot.org>
 * @ref License
 */
// Copyright (c) 2025 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "BenchmarkOptions.h"
#include "Clip.h"
#include "FFmpegReader.h"
#include "FFmpegWriter.h"
#include "Frame.h"
#include "Fraction.h"
#include "FrameMapper.h"
#ifdef USE_IMAGEMAGICK
#include "ImageReader.h"
#else
#include "QtImageReader.h"
#endif
#include "ReaderBase.h"
#include "Settings.h"
#include "Timeline.h"
#include "effects/Brightness.h"
#include "effects/ChromaKey.h"
#include "effects/Crop.h"
#include "effects/AudioVisualization.h"
#include "effects/Mask.h"
#include "effects/Saturation.h"

#include <QImage>

using namespace openshot;
using namespace std;

using Clock = chrono::steady_clock;
using TrialFunc = function<void()>;
using Trial = pair<string, TrialFunc>;

template <typename Func> double time_trial(const string &name, Func func) {
	auto start = Clock::now();
	func();
	auto elapsed =
			chrono::duration_cast<chrono::milliseconds>(Clock::now() - start).count();
	cout << name << "," << elapsed << "\n";
	return static_cast<double>(elapsed);
}

void read_forward_backward(ReaderBase &reader) {
	int64_t len = reader.info.video_length;
	for (int64_t i = 1; i <= len; ++i)
		reader.GetFrame(i);
	for (int64_t i = len; i >= 1; --i)
		reader.GetFrame(i);
}

std::shared_ptr<Frame> make_audio_visualization_frame(int64_t frame_number) {
	const int width = 1280;
	const int height = 720;
	const int sample_rate = 48000;
	const int samples = 1600;
	constexpr double pi = 3.14159265358979323846;

	auto frame = std::make_shared<Frame>(frame_number, width, height, "#00000000", samples, 2);
	auto image = std::make_shared<QImage>(width, height, QImage::Format_RGBA8888_Premultiplied);
	image->fill(Qt::transparent);
	frame->AddImage(image);
	frame->ResizeAudio(2, samples, sample_rate, LAYOUT_STEREO);

	std::vector<float> left(samples);
	std::vector<float> right(samples);
	for (int i = 0; i < samples; ++i) {
		const double t = static_cast<double>(i + frame_number * samples) / sample_rate;
		left[i] = static_cast<float>((std::sin(2.0 * pi * 110.0 * t) * 0.22) +
									 (std::sin(2.0 * pi * 440.0 * t) * 0.48) +
									 (std::sin(2.0 * pi * 1760.0 * t) * 0.18));
		right[i] = static_cast<float>((std::sin(2.0 * pi * 165.0 * t) * 0.18) +
									  (std::sin(2.0 * pi * 660.0 * t) * 0.42) +
									  (std::sin(2.0 * pi * 2640.0 * t) * 0.16));
	}
	frame->AddAudio(true, 0, 0, left.data(), samples, 1.0f);
	frame->AddAudio(true, 1, 0, right.data(), samples, 1.0f);
	return frame;
}

void run_audio_visualization_mode(int mode, int64_t frames) {
	AudioVisualization effect;
	effect.visualization_type = mode;
	effect.background = AUDIO_VISUALIZATION_BACKGROUND_TRANSPARENT;
	effect.detail = Keyframe(0.75);
	effect.glow = Keyframe(0.25);
	effect.intensity = Keyframe(1.25);

	for (int64_t frame_number = 1; frame_number <= frames; ++frame_number)
		effect.GetFrame(make_audio_visualization_frame(frame_number), frame_number);
}

int main(int argc, char* argv[]) {
	const string base = TEST_MEDIA_PATH;
	const string video = base + "sintel_trailer-720p.mp4";
	const string mask_img = base + "mask.png";
	const string overlay = base + "front3.png";
	benchmark::BenchmarkOptions options;
	const int64_t chroma_bench_frames = 500;

	try {
		vector<string> args;
		args.reserve(std::max(0, argc - 1));
		for (int i = 1; i < argc; ++i)
			args.emplace_back(argv[i]);
		options = benchmark::ParseBenchmarkOptions(args);
	} catch (const std::exception& e) {
		cerr << e.what() << "\n";
		cerr << benchmark::BenchmarkUsage() << "\n";
		return 1;
	}

	if (options.show_help) {
		cout << benchmark::BenchmarkUsage() << "\n";
		return 0;
	}

	// Route benchmark thread settings through libopenshot's Settings singleton,
	// matching how an application should configure the library before opening readers.
	Settings *settings = Settings::Instance();
	if (options.omp_threads > 0) {
		settings->OMP_THREADS = options.omp_threads;
		settings->ApplyOpenMPSettings();
	}
	if (options.ff_threads > 0) {
		settings->FF_THREADS = options.ff_threads;
	}

	vector<Trial> trials;
	trials.reserve(10);

	trials.emplace_back("FFmpegReader", [&]() {
		FFmpegReader r(video);
		r.Open();
		read_forward_backward(r);
		r.Close();
	});

	trials.emplace_back("FFmpegWriter", [&]() {
		FFmpegReader r(video);
		r.Open();
		FFmpegWriter w("benchmark_output.mp4");
		w.SetAudioOptions("aac", r.info.sample_rate, 192000);
		w.SetVideoOptions("libx264", r.info.width, r.info.height, r.info.fps,
											5000000);
		w.Open();
		for (int64_t i = 1; i <= r.info.video_length; ++i)
			w.WriteFrame(r.GetFrame(i));
		w.Close();
		r.Close();
	});

	trials.emplace_back("FrameMapper", [&]() {
		vector<Fraction> rates = {Fraction(24, 1), Fraction(30, 1), Fraction(60, 1),
															Fraction(30000, 1001), Fraction(60000, 1001)};
		for (auto &fps : rates) {
			FFmpegReader r(video);
			r.Open();
			FrameMapper map(&r, fps, PULLDOWN_NONE, r.info.sample_rate,
											r.info.channels, r.info.channel_layout);
			map.Open();
			for (int64_t i = 1; i <= map.info.video_length; ++i)
				map.GetFrame(i);
			map.Close();
			r.Close();
		}
	});

	trials.emplace_back("Clip", [&]() {
		Clip c(video);
		c.Open();
		read_forward_backward(c);
		c.Close();
	});

	trials.emplace_back("Timeline", [&]() {
		Timeline t(1920, 1080, Fraction(24, 1), 44100, 2, LAYOUT_STEREO);
		Clip video_clip(video);
		video_clip.Layer(0);
		video_clip.Start(0.0);
		video_clip.End(video_clip.Reader()->info.duration);
		video_clip.Open();
		Clip overlay1(overlay);
		overlay1.Layer(1);
		overlay1.Start(0.0);
		overlay1.End(video_clip.Reader()->info.duration);
		overlay1.Open();
		Clip overlay2(overlay);
		overlay2.Layer(2);
		overlay2.Start(0.0);
		overlay2.End(video_clip.Reader()->info.duration);
		overlay2.Open();
		t.AddClip(&video_clip);
		t.AddClip(&overlay1);
		t.AddClip(&overlay2);
		t.Open();
		t.info.video_length = t.GetMaxFrame();
		read_forward_backward(t);
		t.Close();
	});

	trials.emplace_back("Timeline (with transforms)", [&]() {
		Timeline t(1920, 1080, Fraction(24, 1), 44100, 2, LAYOUT_STEREO);
		Clip video_clip(video);
		int64_t last = video_clip.Reader()->info.video_length;
		video_clip.Layer(0);
		video_clip.Start(0.0);
		video_clip.End(video_clip.Reader()->info.duration);
		video_clip.alpha.AddPoint(1, 1.0);
        video_clip.alpha.AddPoint(last, 0.0);
		video_clip.Open();
		Clip overlay1(overlay);
		overlay1.Layer(1);
		overlay1.Start(0.0);
		overlay1.End(video_clip.Reader()->info.duration);
		overlay1.Open();
		overlay1.scale_x.AddPoint(1, 1.0);
		overlay1.scale_x.AddPoint(last, 0.25);
		overlay1.scale_y.AddPoint(1, 1.0);
		overlay1.scale_y.AddPoint(last, 0.25);
		Clip overlay2(overlay);
		overlay2.Layer(2);
		overlay2.Start(0.0);
		overlay2.End(video_clip.Reader()->info.duration);
		overlay2.Open();
		overlay2.rotation.AddPoint(1, 90.0);
		t.AddClip(&video_clip);
		t.AddClip(&overlay1);
		t.AddClip(&overlay2);
		t.Open();
		t.info.video_length = t.GetMaxFrame();
		read_forward_backward(t);
		t.Close();
	});

	trials.emplace_back("Effect_Mask", [&]() {
		FFmpegReader r(video);
		r.Open();
#ifdef USE_IMAGEMAGICK
		ImageReader mask_reader(mask_img);
#else
		QtImageReader mask_reader(mask_img);
#endif
		mask_reader.Open();
		Clip clip(&r);
		clip.Open();
		Mask m(&mask_reader, Keyframe(0.0), Keyframe(0.5));
		clip.AddEffect(&m);
		read_forward_backward(clip);
		mask_reader.Close();
		clip.Close();
		r.Close();
	});

	trials.emplace_back("Effect_Brightness", [&]() {
		FFmpegReader r(video);
		r.Open();
		Clip clip(&r);
		clip.Open();
		Brightness b(Keyframe(0.5), Keyframe(1.0));
		clip.AddEffect(&b);
		read_forward_backward(clip);
		clip.Close();
		r.Close();
	});

	trials.emplace_back("Effect_Crop", [&]() {
		FFmpegReader r(video);
		r.Open();
		Clip clip(&r);
		clip.Open();
		Crop c(Keyframe(0.25), Keyframe(0.25), Keyframe(0.25), Keyframe(0.25));
		clip.AddEffect(&c);
		read_forward_backward(clip);
		clip.Close();
		r.Close();
	});

	trials.emplace_back("Effect_Saturation", [&]() {
		FFmpegReader r(video);
		r.Open();
		Clip clip(&r);
		clip.Open();
		Saturation s(Keyframe(0.25), Keyframe(0.25), Keyframe(0.25),
								 Keyframe(0.25));
		clip.AddEffect(&s);
		read_forward_backward(clip);
		clip.Close();
		r.Close();
	});

	trials.emplace_back("Effect_ChromaKey_BASIC", [&]() {
		FFmpegReader r(video);
		r.Open();
		Clip clip(&r);
		clip.Open();
		// Default/basic chroma key method baseline
		ChromaKey key(Color(0, 255, 0, 255), Keyframe(80.0), Keyframe(20.0), CHROMAKEY_BASIC);
		clip.AddEffect(&key);
		const int64_t bench_frames = std::min<int64_t>(clip.info.video_length, chroma_bench_frames);
		for (int64_t i = 1; i <= bench_frames; ++i)
			clip.GetFrame(i);
		for (int64_t i = bench_frames; i >= 1; --i)
			clip.GetFrame(i);
		clip.Close();
		r.Close();
	});

	trials.emplace_back("Effect_ChromaKey_BASIC_SOFT", [&]() {
		FFmpegReader r(video);
		r.Open();
		Clip clip(&r);
		clip.Open();
		ChromaKey key(Color(0, 255, 0, 255), Keyframe(80.0), Keyframe(20.0), CHROMAKEY_BASIC_SOFT);
		clip.AddEffect(&key);
		const int64_t bench_frames = std::min<int64_t>(clip.info.video_length, chroma_bench_frames);
		for (int64_t i = 1; i <= bench_frames; ++i)
			clip.GetFrame(i);
		for (int64_t i = bench_frames; i >= 1; --i)
			clip.GetFrame(i);
		clip.Close();
		r.Close();
	});

	const std::vector<std::pair<std::string, int>> audio_visualization_modes = {
		{"Effect_AudioVisualization_Waveform", AUDIO_VISUALIZATION_WAVEFORM},
		{"Effect_AudioVisualization_FilledWaveform", AUDIO_VISUALIZATION_FILLED_WAVEFORM},
		{"Effect_AudioVisualization_Bars", AUDIO_VISUALIZATION_BARS},
		{"Effect_AudioVisualization_Radial", AUDIO_VISUALIZATION_RADIAL},
		{"Effect_AudioVisualization_Spectrum", AUDIO_VISUALIZATION_SPECTRUM},
		{"Effect_AudioVisualization_PhaseScope", AUDIO_VISUALIZATION_PHASE_SCOPE},
		{"Effect_AudioVisualization_Particles", AUDIO_VISUALIZATION_PARTICLES},
		{"Effect_AudioVisualization_VUMeter", AUDIO_VISUALIZATION_VU_METER},
		{"Effect_AudioVisualization_RadialBars", AUDIO_VISUALIZATION_RADIAL_BARS}
	};

	std::transform(audio_visualization_modes.begin(), audio_visualization_modes.end(), std::back_inserter(trials),
		[](const auto& mode) -> Trial {
			return {mode.first, [mode]() {
				run_audio_visualization_mode(mode.second, 240);
			}};
		});

	trials.emplace_back("Effect_AudioVisualization", [audio_visualization_modes]() {
		for (const auto& mode : audio_visualization_modes)
			run_audio_visualization_mode(mode.second, 120);
	});

	trials.emplace_back("Effect_AudioVisualization_SpectrumModes", [&]() {
		const std::vector<int> modes = {
			AUDIO_VISUALIZATION_BARS,
			AUDIO_VISUALIZATION_RADIAL,
			AUDIO_VISUALIZATION_SPECTRUM,
			AUDIO_VISUALIZATION_PARTICLES,
			AUDIO_VISUALIZATION_RADIAL_BARS
		};

		for (int mode : modes)
			run_audio_visualization_mode(mode, 120);
	});

	if (options.list_only) {
		for (const auto& trial : trials)
			cout << trial.first << "\n";
		return 0;
	}

	cout << "Trial,Milliseconds\n";
	double total = 0.0;
	int executed = 0;
	for (const auto& trial : trials) {
		if (!options.filter_test.empty() && trial.first != options.filter_test)
			continue;
		total += time_trial(trial.first, trial.second);
		executed++;
	}

	if (!options.filter_test.empty() && executed == 0) {
		cerr << "Unknown test: " << options.filter_test << "\nAvailable tests:\n";
		for (const auto& trial : trials)
			cerr << "  " << trial.first << "\n";
		return 2;
	}

	cout << "Overall," << total << "\n";
	return 0;
}
