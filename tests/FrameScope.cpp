/**
 * @file
 * @brief Unit tests for FrameScope class
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <memory>
#include <sstream>
#include <vector>

#include <QColor>
#include <QImage>

#include "Frame.h"
#include "FrameScope.h"
#include "openshot_catch.h"

using namespace openshot;

static std::shared_ptr<Frame> makeVideoScopeFrame()
{
	QImage img(4, 1, QImage::Format_ARGB32);
	img.setPixelColor(0, 0, QColor(0, 0, 0, 255));
	img.setPixelColor(1, 0, QColor(255, 0, 0, 255));
	img.setPixelColor(2, 0, QColor(0, 255, 0, 255));
	img.setPixelColor(3, 0, QColor(0, 0, 255, 255));
	auto frame = std::make_shared<Frame>();
	*frame->GetImage() = img;
	return frame;
}

static std::shared_ptr<Frame> makeAudioScopeFrame()
{
	auto frame = std::make_shared<Frame>(1, 8, 2);
	frame->SampleRate(48000);

	const float left[] = {0.0f, 0.5f, -0.25f, 1.0f, -1.0f, 0.25f, -0.5f, 0.0f};
	const float right[] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
	frame->AddAudio(true, 0, 0, left, 8, 1.0f);
	frame->AddAudio(true, 1, 0, right, 8, 1.0f);
	return frame;
}

TEST_CASE("FrameScope returns empty-present flags for missing frame", "[framescope]")
{
	FrameScope scope;
	Json::Value root = scope.JsonValue();

	CHECK(root["version"].asInt() == 1);
	CHECK(root["video"]["present"].asBool() == false);
	CHECK(root["audio"]["present"].asBool() == false);
}

TEST_CASE("FrameScope analyzes video histogram and waveform data", "[framescope][video]")
{
	FrameScope scope(makeVideoScopeFrame(), 4, 4);
	Json::Value root = scope.JsonValue();

	REQUIRE(root["video"]["present"].asBool() == true);
	CHECK(root["video"]["width"].asInt() == 4);
	CHECK(root["video"]["height"].asInt() == 1);
	CHECK(root["video"]["histogram"]["red"].size() == 256);
	CHECK(root["video"]["histogram"]["green"].size() == 256);
	CHECK(root["video"]["histogram"]["blue"].size() == 256);
	CHECK(root["video"]["waveform"]["columns"].asInt() == 4);
	CHECK(root["video"]["waveform"]["bins"].asInt() == 256);
	CHECK(root["video"]["waveform"]["luma"].size() == 4 * 256);
	CHECK(root["video"]["histogram"]["red"][255].asInt() == 1);
	CHECK(root["video"]["histogram"]["green"][255].asInt() == 1);
	CHECK(root["video"]["histogram"]["blue"][255].asInt() == 1);
	CHECK(root["video"]["histogram"]["luma"][0].asInt() == 1);
	CHECK(root["video"]["summary"]["avg_luma"].asDouble() > 0.2);
}

TEST_CASE("FrameScope analyzes audio waveform and summary data", "[framescope][audio]")
{
	FrameScope scope(makeAudioScopeFrame(), 4, 4);
	Json::Value root = scope.JsonValue();

	REQUIRE(root["audio"]["present"].asBool() == true);
	CHECK(root["audio"]["channels"].asInt() == 2);
	CHECK(root["audio"]["samples"].asInt() == 8);
	CHECK(root["audio"]["sample_rate"].asInt() == 48000);
	CHECK(root["audio"]["waveform"]["buckets"].asInt() == 4);
	CHECK(root["audio"]["waveform"]["min"].size() == 2);
	CHECK(root["audio"]["waveform"]["max"].size() == 2);
	CHECK(root["audio"]["waveform"]["min"][0].size() == 4);
	CHECK(root["audio"]["waveform"]["max"][1].size() == 4);
	CHECK(root["audio"]["summary"]["peak"][0].asFloat() == Approx(1.0f));
	CHECK(root["audio"]["summary"]["peak"][1].asFloat() == Approx(0.8f));
	CHECK(root["audio"]["summary"]["clipped_samples"][0].asInt() == 2);
	CHECK(root["audio"]["summary"]["clipped_samples"][1].asInt() == 0);
	CHECK(root["audio"]["summary"]["rms"][0].asFloat() > 0.0f);
}

TEST_CASE("FrameScope Json string parses cleanly", "[framescope][json]")
{
	FrameScope scope(makeAudioScopeFrame(), 8, 4);
	std::istringstream json_stream(scope.Json());
	Json::CharReaderBuilder rb;
	Json::Value root;
	std::string errs;

	REQUIRE(Json::parseFromStream(rb, json_stream, &root, &errs));
	CHECK(root["audio"]["present"].asBool() == true);
}
