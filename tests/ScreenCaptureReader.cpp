/**
 * @file
 * @brief Unit tests for openshot::ScreenCaptureReader settings and metadata
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "openshot_catch.h"

#include "Exceptions.h"
#include "ScreenCaptureReader.h"

using namespace openshot;

TEST_CASE("Screen capture settings validation", "[libopenshot][screencapturereader]")
{
#if defined(__linux__)
	ScreenCaptureSettings settings;
	settings.backend = SCREEN_CAPTURE_X11;
	settings.display = ":0.0";
	settings.width = 640;
	settings.height = 360;
	settings.fps = Fraction(30, 1);

	CHECK_NOTHROW([&settings]() { ScreenCaptureReader reader(settings); }());

	settings.width = 0;
	CHECK_THROWS_AS([&settings]() { ScreenCaptureReader reader(settings); }(), InvalidOptions);

	settings.width = 640;
	settings.fps = Fraction(0, 1);
	CHECK_THROWS_AS([&settings]() { ScreenCaptureReader reader(settings); }(), InvalidOptions);
#else
	CHECK_FALSE(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_X11));
#endif
}

TEST_CASE("Screen capture reader reports configured video info", "[libopenshot][screencapturereader]")
{
#if defined(__linux__)
	ScreenCaptureSettings settings;
	settings.backend = SCREEN_CAPTURE_X11;
	settings.display = ":99.0";
	settings.x = 10;
	settings.y = 20;
	settings.width = 800;
	settings.height = 450;
	settings.fps = Fraction(25, 1);
	settings.include_cursor = false;
	settings.show_region = true;
	settings.options["window_id"] = "12345";

	ScreenCaptureReader reader(settings);
	CHECK(reader.Name() == "ScreenCaptureReader");
	CHECK(reader.info.has_video == true);
	CHECK(reader.info.has_audio == false);
	CHECK(reader.info.width == 800);
	CHECK(reader.info.height == 450);
	CHECK(reader.info.fps.num == 25);
	CHECK(reader.info.fps.den == 1);

	const Json::Value json = reader.JsonValue();
	CHECK(json["type"].asString() == "ScreenCaptureReader");
	CHECK(json["display"].asString() == ":99.0");
	CHECK(json["x"].asInt() == 10);
	CHECK(json["y"].asInt() == 20);
	CHECK(json["include_cursor"].asBool() == false);
	CHECK(json["show_region"].asBool() == true);
	CHECK(json["options"]["window_id"].asString() == "12345");
#else
	CHECK_FALSE(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_X11));
#endif
}
