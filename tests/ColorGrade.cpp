/**
 * @file
 * @brief Unit tests for ColorGrade effect
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <memory>
#include <QColor>
#include <QImage>
#include <sstream>

#include "Frame.h"
#include "effects/ColorGrade.h"
#include "openshot_catch.h"

using namespace openshot;

static std::shared_ptr<Frame> makeGradeFrame()
{
	QImage img(2, 2, QImage::Format_ARGB32);
	img.fill(QColor(80, 90, 110, 255));
	img.setPixelColor(0, 0, QColor(64, 96, 160, 255));
	auto frame = std::make_shared<Frame>();
	*frame->GetImage() = img;
	return frame;
}

static std::shared_ptr<Frame> makeTransparentGradeFrame()
{
	QImage img(2, 2, QImage::Format_ARGB32_Premultiplied);
	img.fill(QColor(40, 50, 70, 128));
	img.setPixelColor(0, 0, QColor(64, 96, 128, 128));
	auto frame = std::make_shared<Frame>();
	*frame->GetImage() = img;
	return frame;
}

static std::string lutPath()
{
	std::stringstream path;
	path << TEST_MEDIA_PATH << "example-lut.cube";
	return path.str();
}

TEST_CASE("Default ColorGrade leaves image unchanged", "[effect][colorgrade]")
{
	ColorGrade effect;
	auto frame = makeGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);
	CHECK(after == before);
}

TEST_CASE("ColorGrade JSON round-trip preserves wheels and curves", "[effect][colorgrade][json]")
{
	ColorGrade effect;
	effect.temperature = Keyframe(0.25);
	effect.mix = Keyframe(0.75);
	Json::Value wheels(Json::objectValue);
	wheels["enabled"] = false;
	wheels["global"]["color"] = "#ff8800";
	wheels["global"]["amount"] = 0.4;
	wheels["global"]["luma"] = 0.1;
	effect.wheels.SetJsonValue(wheels);

	Json::Value curve(Json::objectValue);
	curve["enabled"] = false;
	curve["points"] = Json::Value(Json::arrayValue);
	Json::Value p0(Json::objectValue);
	p0["x"] = 0.0;
	p0["y"] = 0.0;
	Json::Value p1(Json::objectValue);
	p1["x"] = 0.5;
	p1["y"] = 0.75;
	Json::Value p2(Json::objectValue);
	p2["x"] = 1.0;
	p2["y"] = 1.0;
	curve["points"].append(p0);
	curve["points"].append(p1);
	curve["points"].append(p2);
	effect.curve_master.SetJsonValue(curve);
	effect.SetJsonValue(Json::Value(Json::objectValue));

	ColorGrade copy;
	copy.SetJson(effect.Json());

	CHECK(copy.temperature.GetValue(0) == Approx(0.25));
	CHECK(copy.mix.GetValue(0) == Approx(0.75));
	CHECK(copy.JsonValue()["wheels"]["enabled"].asBool() == false);
	CHECK(copy.JsonValue()["wheels"]["global"]["color"].asString() == "#ff8800");
	CHECK(copy.JsonValue()["curve_master"]["enabled"].asBool() == false);
	CHECK(copy.JsonValue()["curve_master"]["points"].size() == 3);
}

TEST_CASE("ColorGrade defaults missing enabled flags to true", "[effect][colorgrade][json]")
{
	ColorGrade effect;

	Json::Value wheels(Json::objectValue);
	wheels["global"]["color"] = "#3366ff";
	wheels["global"]["amount"] = 0.5;
	effect.wheels.SetJsonValue(wheels);

	Json::Value curve(Json::objectValue);
	curve["points"] = Json::Value(Json::arrayValue);
	Json::Value p0(Json::objectValue);
	p0["x"] = 0.0;
	p0["y"] = 0.0;
	Json::Value p1(Json::objectValue);
	p1["x"] = 1.0;
	p1["y"] = 1.0;
	curve["points"].append(p0);
	curve["points"].append(p1);
	effect.curve_master.SetJsonValue(curve);

	CHECK(effect.wheels.JsonValue()["enabled"].asBool() == true);
	CHECK(effect.curve_master.JsonValue()["enabled"].asBool() == true);
}

TEST_CASE("PropertiesJSON exposes rich wheels and curve properties", "[effect][colorgrade][ui]")
{
	ColorGrade effect;
	std::istringstream props(effect.PropertiesJSON(1));
	Json::CharReaderBuilder rb;
	Json::Value root;
	std::string errs;
	REQUIRE(Json::parseFromStream(rb, props, &root, &errs));
	CHECK(root.isMember("wheels"));
	CHECK(root["wheels"]["type"].asString() == "colorgrade_wheels");
	CHECK(root["wheels"].isMember("wheels"));
	CHECK(root["wheels"]["wheels"]["enabled"].asBool() == true);
	CHECK(root.isMember("curve_master"));
	CHECK(root["curve_master"]["type"].asString() == "colorgrade_curve");
	CHECK(root["curve_master"].isMember("curve"));
	CHECK(root["curve_master"]["curve"]["enabled"].asBool() == true);
}

TEST_CASE("Curve data brightens matching pixels", "[effect][colorgrade][curve]")
{
	ColorGrade effect;
	Json::Value curve(Json::objectValue);
	curve["points"] = Json::Value(Json::arrayValue);
	Json::Value p0(Json::objectValue);
	p0["x"] = 0.0;
	p0["y"] = 0.0;
	Json::Value p1(Json::objectValue);
	p1["x"] = 0.5;
	p1["y"] = 0.8;
	Json::Value p2(Json::objectValue);
	p2["x"] = 1.0;
	p2["y"] = 1.0;
	curve["points"].append(p0);
	curve["points"].append(p1);
	curve["points"].append(p2);
	effect.curve_master.SetJsonValue(curve);

	auto frame = makeGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);
	CHECK(after.red() > before.red());
}

TEST_CASE("Disabled curve preserves points but renders as identity", "[effect][colorgrade][curve]")
{
	ColorGrade effect;
	Json::Value curve(Json::objectValue);
	curve["enabled"] = false;
	curve["points"] = Json::Value(Json::arrayValue);
	Json::Value p0(Json::objectValue);
	p0["x"] = 0.0;
	p0["y"] = 0.2;
	Json::Value p1(Json::objectValue);
	p1["x"] = 0.5;
	p1["y"] = 0.9;
	Json::Value p2(Json::objectValue);
	p2["x"] = 1.0;
	p2["y"] = 1.0;
	curve["points"].append(p0);
	curve["points"].append(p1);
	curve["points"].append(p2);
	effect.curve_master.SetJsonValue(curve);

	auto frame = makeGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);

	CHECK(after == before);
	CHECK(effect.curve_master.JsonValue()["enabled"].asBool() == false);
	CHECK(effect.curve_master.JsonValue()["points"].size() == 3);
}

TEST_CASE("Wheel data shifts tonal balance", "[effect][colorgrade][wheels]")
{
	ColorGrade effect;
	Json::Value wheels(Json::objectValue);
	wheels["shadows"]["color"] = "#ff6600";
	wheels["shadows"]["amount"] = 0.8;
	wheels["shadows"]["luma"] = 0.1;
	effect.wheels.SetJsonValue(wheels);

	auto frame = makeGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);
	CHECK(after != before);
}

TEST_CASE("Disabled wheels preserve values but render as identity", "[effect][colorgrade][wheels]")
{
	ColorGrade effect;
	Json::Value wheels(Json::objectValue);
	wheels["enabled"] = false;
	wheels["shadows"]["color"] = "#ff6600";
	wheels["shadows"]["amount"] = 0.8;
	wheels["shadows"]["luma"] = 0.1;
	effect.wheels.SetJsonValue(wheels);

	auto frame = makeGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);

	CHECK(after == before);
	CHECK(effect.wheels.JsonValue()["enabled"].asBool() == false);
	CHECK(effect.wheels.JsonValue()["shadows"]["amount"].asFloat() == Approx(0.8f));
}

TEST_CASE("Identity ColorGrade preserves semi-transparent pixels", "[effect][colorgrade][alpha]")
{
	ColorGrade effect;
	auto frame = makeTransparentGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);

	CHECK(after == before);
}

TEST_CASE("LUT path and intensity reuse ColorMap behavior", "[effect][colorgrade][lut]")
{
	ColorGrade effect;
	Json::Value update(Json::objectValue);
	update["lut_path"] = lutPath();
	update["lut_intensity"] = Keyframe(1.0).JsonValue();
	effect.SetJsonValue(update);

	auto frame = makeGradeFrame();
	const QColor before = frame->GetImage()->pixelColor(0, 0);
	auto out = effect.GetFrame(frame, 1);
	const QColor after = out->GetImage()->pixelColor(0, 0);
	CHECK(after != before);
}
