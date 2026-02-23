/**
 * @file
 * @brief Unit tests for common EffectBase mask dispatch and blur mask modes
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include <QColor>
#include <QDir>
#include <QImage>

#include "effects/Blur.h"
#include "effects/Brightness.h"
#include "effects/Hue.h"
#include "effects/Pixelate.h"
#include "effects/Saturation.h"
#include "effects/Sharpen.h"
#include "QtImageReader.h"
#include "openshot_catch.h"

using namespace openshot;

static std::string temp_png_path(const std::string& base) {
	std::stringstream path;
	path << QDir::tempPath().toStdString() << "/libopenshot_" << base << "_"
		 << getpid() << "_" << rand() << ".png";
	return path.str();
}

static std::string create_source_png(int w, int h, const QColor& color) {
	const std::string path = temp_png_path("source");
	QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
	image.fill(color);
	REQUIRE(image.save(QString::fromStdString(path)));
	return path;
}

static std::string create_mask_png(const std::vector<int>& gray_values) {
	const std::string path = temp_png_path("mask");
	QImage mask(static_cast<int>(gray_values.size()), 1, QImage::Format_RGBA8888_Premultiplied);
	for (size_t i = 0; i < gray_values.size(); ++i) {
		const int gray = gray_values[i];
		mask.setPixelColor(static_cast<int>(i), 0, QColor(gray, gray, gray, 255));
	}
	REQUIRE(mask.save(QString::fromStdString(path)));
	return path;
}

static std::string create_uniform_mask_png(int width, int height, int gray_value) {
	const std::string path = temp_png_path("mask_uniform");
	QImage mask(width, height, QImage::Format_RGBA8888_Premultiplied);
	mask.fill(QColor(gray_value, gray_value, gray_value, 255));
	REQUIRE(mask.save(QString::fromStdString(path)));
	return path;
}

TEST_CASE("EffectBase common mask blend applies to ProcessFrame", "[effect][mask][base]") {
	auto frame = std::make_shared<Frame>(1, 4, 1, "#000000");
	auto image = frame->GetImage();
	image->fill(QColor(80, 80, 80, 255));

	const std::string mask_path = create_mask_png({255, 255, 0, 0});

	Brightness effect(Keyframe(0.5), Keyframe(0.0));
	effect.MaskReader(new QtImageReader(mask_path));

	auto out = effect.ProcessFrame(frame, 1);
	auto out_image = out->GetImage();

	CHECK(out_image->pixelColor(0, 0).red() > 80);
	CHECK(out_image->pixelColor(1, 0).red() > 80);
	CHECK(out_image->pixelColor(2, 0).red() == 80);
	CHECK(out_image->pixelColor(3, 0).red() == 80);
}

TEST_CASE("EffectBase mask fields serialize and deserialize", "[effect][mask][json]") {
	const std::string mask_path = create_mask_png({255, 0});

	Brightness effect(Keyframe(0.0), Keyframe(0.0));
	effect.mask_invert = true;
	effect.MaskReader(new QtImageReader(mask_path));

	const Json::Value json = effect.JsonValue();
	CHECK(json["mask_invert"].asBool());
	REQUIRE(json["mask_reader"].isObject());
	CHECK(json["mask_reader"]["type"].asString() == "QtImageReader");

	Brightness copy(Keyframe(0.0), Keyframe(0.0));
	copy.SetJsonValue(json);
	CHECK(copy.mask_invert);
	CHECK(copy.MaskReader() != nullptr);
}

TEST_CASE("Blur mask mode drive amount differs from post blend", "[effect][mask][blur]") {
	const int width = 20;
	const int height = 20;
	auto frame_post = std::make_shared<Frame>(1, width, height, "#000000");
	auto frame_drive = std::make_shared<Frame>(1, width, height, "#000000");

	QImage input(width, height, QImage::Format_RGBA8888_Premultiplied);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const int alpha = std::min(255, x * 12 + y * 3);
			input.setPixelColor(x, y, QColor(255, 180, 40, alpha));
		}
	}
	*frame_post->GetImage() = input;
	*frame_drive->GetImage() = input;

	const std::string mask_path = create_uniform_mask_png(width, height, 128);

	Blur post(Keyframe(3.0), Keyframe(3.0), Keyframe(3.0), Keyframe(1.0));
	post.mask_mode = BLUR_MASK_POST_BLEND;
	post.MaskReader(new QtImageReader(mask_path));

	Blur drive(Keyframe(3.0), Keyframe(3.0), Keyframe(3.0), Keyframe(1.0));
	drive.mask_mode = BLUR_MASK_DRIVE_AMOUNT;
	drive.MaskReader(new QtImageReader(mask_path));

	auto out_post = post.ProcessFrame(frame_post, 1);
	auto out_drive = drive.ProcessFrame(frame_drive, 1);

	bool any_diff = false;
	for (int y = 0; y < height && !any_diff; ++y) {
		for (int x = 0; x < width; ++x) {
			if (out_post->GetImage()->pixelColor(x, y) != out_drive->GetImage()->pixelColor(x, y)) {
				any_diff = true;
				break;
			}
		}
	}
	if (!any_diff) {
		for (int x = 0; x < width; ++x) {
			if (out_post->GetImage()->pixelColor(x, height / 2) != out_drive->GetImage()->pixelColor(x, height / 2)) {
			any_diff = true;
			break;
		}
	}
	}
	CHECK(any_diff);
}

TEST_CASE("Saturation mask mode drive amount differs from post blend", "[effect][mask][saturation]") {
	auto frame_post = std::make_shared<Frame>(1, 1, 1, "#000000");
	auto frame_drive = std::make_shared<Frame>(1, 1, 1, "#000000");

	QImage input(1, 1, QImage::Format_RGBA8888_Premultiplied);
	input.setPixelColor(0, 0, QColor(70, 120, 200, 255));
	*frame_post->GetImage() = input;
	*frame_drive->GetImage() = input;

	const std::string mask_path = create_mask_png({128});

	Saturation post(Keyframe(2.0), Keyframe(1.0), Keyframe(1.0), Keyframe(1.0));
	post.mask_mode = SATURATION_MASK_POST_BLEND;
	post.MaskReader(new QtImageReader(mask_path));

	Saturation drive(Keyframe(2.0), Keyframe(1.0), Keyframe(1.0), Keyframe(1.0));
	drive.mask_mode = SATURATION_MASK_DRIVE_AMOUNT;
	drive.MaskReader(new QtImageReader(mask_path));

	auto out_post = post.ProcessFrame(frame_post, 1);
	auto out_drive = drive.ProcessFrame(frame_drive, 1);

	CHECK(out_post->GetImage()->pixelColor(0, 0) != out_drive->GetImage()->pixelColor(0, 0));
}

TEST_CASE("Brightness mask mode vary strength differs from limit-to-area", "[effect][mask][brightness]") {
	auto frame_post = std::make_shared<Frame>(1, 1, 1, "#000000");
	auto frame_drive = std::make_shared<Frame>(1, 1, 1, "#000000");

	QImage input(1, 1, QImage::Format_RGBA8888_Premultiplied);
	input.setPixelColor(0, 0, QColor(80, 120, 200, 255));
	*frame_post->GetImage() = input;
	*frame_drive->GetImage() = input;

	const std::string mask_path = create_mask_png({128});

	Brightness post(Keyframe(0.6), Keyframe(6.0));
	post.mask_mode = BRIGHTNESS_MASK_LIMIT_TO_AREA;
	post.MaskReader(new QtImageReader(mask_path));

	Brightness drive(Keyframe(0.6), Keyframe(6.0));
	drive.mask_mode = BRIGHTNESS_MASK_VARY_STRENGTH;
	drive.MaskReader(new QtImageReader(mask_path));

	auto out_post = post.ProcessFrame(frame_post, 1);
	auto out_drive = drive.ProcessFrame(frame_drive, 1);

	CHECK(out_post->GetImage()->pixelColor(0, 0) != out_drive->GetImage()->pixelColor(0, 0));
}

TEST_CASE("Hue mask mode vary strength differs from limit-to-area", "[effect][mask][hue]") {
	auto frame_post = std::make_shared<Frame>(1, 1, 1, "#000000");
	auto frame_drive = std::make_shared<Frame>(1, 1, 1, "#000000");

	QImage input(1, 1, QImage::Format_RGBA8888_Premultiplied);
	input.setPixelColor(0, 0, QColor(200, 80, 40, 255));
	*frame_post->GetImage() = input;
	*frame_drive->GetImage() = input;

	const std::string mask_path = create_mask_png({128});

	Hue post(Keyframe(0.8));
	post.mask_mode = HUE_MASK_LIMIT_TO_AREA;
	post.MaskReader(new QtImageReader(mask_path));

	Hue drive(Keyframe(0.8));
	drive.mask_mode = HUE_MASK_VARY_STRENGTH;
	drive.MaskReader(new QtImageReader(mask_path));

	auto out_post = post.ProcessFrame(frame_post, 1);
	auto out_drive = drive.ProcessFrame(frame_drive, 1);

	CHECK(out_post->GetImage()->pixelColor(0, 0) != out_drive->GetImage()->pixelColor(0, 0));
}

TEST_CASE("Pixelate mask mode vary strength differs from limit-to-area", "[effect][mask][pixelate]") {
	const int width = 20;
	const int height = 20;
	auto frame_post = std::make_shared<Frame>(1, width, height, "#000000");
	auto frame_drive = std::make_shared<Frame>(1, width, height, "#000000");

	QImage input(width, height, QImage::Format_RGBA8888_Premultiplied);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			input.setPixelColor(x, y, QColor((x * 13) % 256, (y * 11) % 256, ((x + y) * 7) % 256, 255));
		}
	}
	*frame_post->GetImage() = input;
	*frame_drive->GetImage() = input;

	const std::string mask_path = create_uniform_mask_png(width, height, 128);

	Pixelate post(Keyframe(1.0), Keyframe(0.0), Keyframe(0.0), Keyframe(0.0), Keyframe(0.0));
	post.mask_mode = PIXELATE_MASK_LIMIT_TO_AREA;
	post.MaskReader(new QtImageReader(mask_path));

	Pixelate drive(Keyframe(1.0), Keyframe(0.0), Keyframe(0.0), Keyframe(0.0), Keyframe(0.0));
	drive.mask_mode = PIXELATE_MASK_VARY_STRENGTH;
	drive.MaskReader(new QtImageReader(mask_path));

	auto out_post = post.ProcessFrame(frame_post, 1);
	auto out_drive = drive.ProcessFrame(frame_drive, 1);

	bool any_diff = false;
	for (int y = 0; y < height && !any_diff; ++y) {
		for (int x = 0; x < width; ++x) {
			if (out_post->GetImage()->pixelColor(x, y) != out_drive->GetImage()->pixelColor(x, y)) {
				any_diff = true;
				break;
			}
		}
	}
	CHECK(any_diff);
}

TEST_CASE("Sharpen mask mode vary strength differs from limit-to-area", "[effect][mask][sharpen]") {
	const int width = 20;
	const int height = 20;
	auto frame_post = std::make_shared<Frame>(1, width, height, "#000000");
	auto frame_drive = std::make_shared<Frame>(1, width, height, "#000000");

	QImage input(width, height, QImage::Format_RGBA8888_Premultiplied);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const int red = (x < width / 2) ? 30 : 220;
			const int green = (y < height / 2) ? 60 : 200;
			const int blue = ((x + y) % 2 == 0) ? 20 : 240;
			input.setPixelColor(x, y, QColor(red, green, blue, 255));
		}
	}
	*frame_post->GetImage() = input;
	*frame_drive->GetImage() = input;

	const std::string mask_path = create_uniform_mask_png(width, height, 128);

	Sharpen post(Keyframe(1.6), Keyframe(3.0), Keyframe(0.05));
	post.mask_mode = SHARPEN_MASK_LIMIT_TO_AREA;
	post.MaskReader(new QtImageReader(mask_path));

	Sharpen drive(Keyframe(1.6), Keyframe(3.0), Keyframe(0.05));
	drive.mask_mode = SHARPEN_MASK_VARY_STRENGTH;
	drive.MaskReader(new QtImageReader(mask_path));

	auto out_post = post.ProcessFrame(frame_post, 1);
	auto out_drive = drive.ProcessFrame(frame_drive, 1);

	bool any_diff = false;
	for (int y = 0; y < height && !any_diff; ++y) {
		for (int x = 0; x < width; ++x) {
			if (out_post->GetImage()->pixelColor(x, y) != out_drive->GetImage()->pixelColor(x, y)) {
				any_diff = true;
				break;
			}
		}
	}
	CHECK(any_diff);
}

TEST_CASE("Effect mask_mode roundtrip for supported effects", "[effect][mask][mode][json]") {
	Blur blur(Keyframe(1.0), Keyframe(1.0), Keyframe(3.0), Keyframe(1.0));
	blur.mask_mode = BLUR_MASK_DRIVE_AMOUNT;
	Blur blur_copy;
	blur_copy.SetJsonValue(blur.JsonValue());
	CHECK(blur_copy.mask_mode == BLUR_MASK_DRIVE_AMOUNT);

	Saturation saturation(Keyframe(1.0), Keyframe(1.0), Keyframe(1.0), Keyframe(1.0));
	saturation.mask_mode = SATURATION_MASK_DRIVE_AMOUNT;
	Saturation saturation_copy;
	saturation_copy.SetJsonValue(saturation.JsonValue());
	CHECK(saturation_copy.mask_mode == SATURATION_MASK_DRIVE_AMOUNT);

	Brightness brightness(Keyframe(0.4), Keyframe(5.0));
	brightness.mask_mode = BRIGHTNESS_MASK_VARY_STRENGTH;
	Brightness brightness_copy;
	brightness_copy.SetJsonValue(brightness.JsonValue());
	CHECK(brightness_copy.mask_mode == BRIGHTNESS_MASK_VARY_STRENGTH);

	Hue hue(Keyframe(0.4));
	hue.mask_mode = HUE_MASK_VARY_STRENGTH;
	Hue hue_copy;
	hue_copy.SetJsonValue(hue.JsonValue());
	CHECK(hue_copy.mask_mode == HUE_MASK_VARY_STRENGTH);

	Pixelate pixelate(Keyframe(0.8), Keyframe(0.0), Keyframe(0.0), Keyframe(0.0), Keyframe(0.0));
	pixelate.mask_mode = PIXELATE_MASK_VARY_STRENGTH;
	Pixelate pixelate_copy;
	pixelate_copy.SetJsonValue(pixelate.JsonValue());
	CHECK(pixelate_copy.mask_mode == PIXELATE_MASK_VARY_STRENGTH);

	Sharpen sharpen(Keyframe(1.5), Keyframe(2.0), Keyframe(0.1));
	sharpen.mask_mode = SHARPEN_MASK_VARY_STRENGTH;
	Sharpen sharpen_copy;
	sharpen_copy.SetJsonValue(sharpen.JsonValue());
	CHECK(sharpen_copy.mask_mode == SHARPEN_MASK_VARY_STRENGTH);
}

TEST_CASE("EffectBase accepts legacy reader key for mask source", "[effect][mask][json][legacy_reader]") {
	const std::string mask_path = create_mask_png({255, 0});
	QtImageReader reader(mask_path);

	Saturation effect(Keyframe(2.0), Keyframe(1.0), Keyframe(1.0), Keyframe(1.0));
	Json::Value update;
	update["reader"] = reader.JsonValue();
	effect.SetJsonValue(update);

	REQUIRE(effect.MaskReader() != nullptr);
	CHECK(effect.JsonValue()["mask_reader"]["type"].asString() == "QtImageReader");
}
