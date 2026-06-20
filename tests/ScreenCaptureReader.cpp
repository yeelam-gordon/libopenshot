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

#include <cstdlib>
#include <string>

using namespace openshot;

TEST_CASE("Screen capture settings validation", "[libopenshot][screencapturereader]")
{
	ScreenCaptureSettings settings;
#if defined(__linux__)
	settings.backend = SCREEN_CAPTURE_X11;
	settings.display = ":0.0";
#elif defined(_WIN32)
	settings.backend = SCREEN_CAPTURE_WINDOWS_GDI;
	settings.display = "desktop";
#endif
#if defined(__linux__) || defined(_WIN32)
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
	ScreenCaptureSettings settings;
#if defined(__linux__)
	settings.backend = SCREEN_CAPTURE_X11;
	settings.display = ":99.0";
#elif defined(_WIN32)
	settings.backend = SCREEN_CAPTURE_WINDOWS_GDI;
	settings.display = "desktop";
#endif
#if defined(__linux__) || defined(_WIN32)
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
#if defined(__linux__)
	CHECK(json["options"]["window_id"].asString() == "12345");
#endif
#else
	CHECK_FALSE(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_X11));
#endif
}

TEST_CASE("Screen capture backend support follows platform build features", "[libopenshot][screencapturereader]")
{
#if defined(__linux__)
	CHECK(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_AUTO));
	CHECK(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_X11));

	ScreenCaptureSettings wayland_settings;
	wayland_settings.backend = SCREEN_CAPTURE_WAYLAND;
	wayland_settings.width = 640;
	wayland_settings.height = 360;

	if (ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_WAYLAND)) {
		CHECK_NOTHROW([&wayland_settings]() { ScreenCaptureReader reader(wayland_settings); }());
	} else {
		CHECK_THROWS_AS([&wayland_settings]() { ScreenCaptureReader reader(wayland_settings); }(), InvalidOptions);
	}
#else
#if defined(_WIN32)
	CHECK(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_AUTO));
	CHECK(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_WINDOWS_GDI));
#else
	CHECK_FALSE(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_AUTO));
#endif
	CHECK_FALSE(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_X11));
	CHECK_FALSE(ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_WAYLAND));
#endif
}

TEST_CASE("Screen capture default backend follows platform", "[libopenshot][screencapturereader]")
{
#if defined(_WIN32)
	CHECK(ScreenCaptureReader::DefaultBackend() == SCREEN_CAPTURE_WINDOWS_GDI);
#endif
}

TEST_CASE("Screen capture auto backend prefers Wayland only when supported", "[libopenshot][screencapturereader]")
{
#if defined(__linux__)
	const char* previous_session = std::getenv("XDG_SESSION_TYPE");
	const std::string previous_value = previous_session ? previous_session : "";
	setenv("XDG_SESSION_TYPE", "wayland", 1);

	const ScreenCaptureBackend default_backend = ScreenCaptureReader::DefaultBackend();
	if (ScreenCaptureReader::IsBackendSupported(SCREEN_CAPTURE_WAYLAND)) {
		CHECK(default_backend == SCREEN_CAPTURE_WAYLAND);
	} else {
		CHECK(default_backend == SCREEN_CAPTURE_AUTO);
	}

	if (previous_session) {
		setenv("XDG_SESSION_TYPE", previous_value.c_str(), 1);
	} else {
		unsetenv("XDG_SESSION_TYPE");
	}
#endif
}
