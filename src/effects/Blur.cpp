/**
 * @file
 * @brief Source file for Blur effect class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2019 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "Blur.h"
#include "Exceptions.h"

using namespace openshot;

/// Blank constructor, useful when using Json to load the effect properties
Blur::Blur() : horizontal_radius(6.0), vertical_radius(6.0), sigma(3.0), iterations(3.0),
	mask_mode(BLUR_MASK_POST_BLEND) {
	// Init effect properties
	init_effect_details();
}

// Default constructor
Blur::Blur(Keyframe new_horizontal_radius, Keyframe new_vertical_radius, Keyframe new_sigma, Keyframe new_iterations) :
		horizontal_radius(new_horizontal_radius), vertical_radius(new_vertical_radius),
		sigma(new_sigma), iterations(new_iterations), mask_mode(BLUR_MASK_POST_BLEND)
{
	// Init effect properties
	init_effect_details();
}

// Init effect settings
void Blur::init_effect_details()
{
	/// Initialize the values of the EffectInfo struct.
	InitEffectInfo();

	/// Set the effect info
	info.class_name = "Blur";
	info.name = "Blur";
	info.description = "Adjust the blur of the frame's image.";
	info.has_audio = false;
	info.has_video = true;
}

// This method is required for all derived classes of EffectBase, and returns a
// modified openshot::Frame object
std::shared_ptr<openshot::Frame> Blur::GetFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number)
{
	// Get the frame's image
	std::shared_ptr<QImage> frame_image = frame->GetImage();

	// Get the current blur radius
	int horizontal_radius_value = horizontal_radius.GetValue(frame_number);
	int vertical_radius_value = vertical_radius.GetValue(frame_number);
	float sigma_value = sigma.GetValue(frame_number);
	int iteration_value = iterations.GetInt(frame_number);
	(void) sigma_value;

	int w = frame_image->width();
	int h = frame_image->height();

	// Grab two copies of the image pixel data
	QImage image_copy = frame_image->copy();
	std::shared_ptr<QImage> frame_image_2 = std::make_shared<QImage>(image_copy);

	// Loop through each iteration
	for (int iteration = 0; iteration < iteration_value; ++iteration)
	{
		// HORIZONTAL BLUR (if any)
		if (horizontal_radius_value > 0.0) {
			// Apply horizontal blur to target RGBA channels
			boxBlurH(frame_image->bits(), frame_image_2->bits(), w, h, horizontal_radius_value);

			// Swap output image back to input
			frame_image.swap(frame_image_2);
		}

		// VERTICAL BLUR (if any)
		if (vertical_radius_value > 0.0) {
			// Apply vertical blur to target RGBA channels
			boxBlurT(frame_image->bits(), frame_image_2->bits(), w, h, vertical_radius_value);

			// Swap output image back to input
			frame_image.swap(frame_image_2);
		}
	}

	// return the modified frame
	return frame;
}

bool Blur::UseCustomMaskBlend(int64_t frame_number) const {
	(void) frame_number;
	return mask_mode == BLUR_MASK_DRIVE_AMOUNT;
}

void Blur::ApplyCustomMaskBlend(std::shared_ptr<QImage> original_image, std::shared_ptr<QImage> effected_image,
								std::shared_ptr<QImage> mask_image, int64_t frame_number) const {
	(void) frame_number;
	if (!original_image || !effected_image || !mask_image)
		return;
	if (original_image->size() != effected_image->size() || effected_image->size() != mask_image->size())
		return;

	unsigned char* original_pixels = reinterpret_cast<unsigned char*>(original_image->bits());
	unsigned char* effected_pixels = reinterpret_cast<unsigned char*>(effected_image->bits());
	unsigned char* mask_pixels = reinterpret_cast<unsigned char*>(mask_image->bits());
	const int pixel_count = effected_image->width() * effected_image->height();

	#pragma omp parallel for schedule(static)
	for (int i = 0; i < pixel_count; ++i) {
		const int idx = i * 4;
		float factor = static_cast<float>(qGray(mask_pixels[idx], mask_pixels[idx + 1], mask_pixels[idx + 2])) / 255.0f;
		if (mask_invert)
			factor = 1.0f - factor;
		// Use a non-linear response curve for custom blur drive mode.
		factor = factor * factor;
		const float inverse = 1.0f - factor;

		// Drive blur amount with the grayscale mask while preserving source alpha.
		effected_pixels[idx] = static_cast<unsigned char>(
			(original_pixels[idx] * inverse) + (effected_pixels[idx] * factor));
		effected_pixels[idx + 1] = static_cast<unsigned char>(
			(original_pixels[idx + 1] * inverse) + (effected_pixels[idx + 1] * factor));
		effected_pixels[idx + 2] = static_cast<unsigned char>(
			(original_pixels[idx + 2] * inverse) + (effected_pixels[idx + 2] * factor));
		effected_pixels[idx + 3] = original_pixels[idx + 3];
	}
}

// Credit: http://blog.ivank.net/fastest-gaussian-blur.html (MIT License)
// Modified to process all four channels in a pixel array
void Blur::boxBlurH(unsigned char *scl, unsigned char *tcl, int w, int h, int r) {
	float iarr = 1.0 / (r + r + 1);

	#pragma omp parallel for shared (scl, tcl)
	for (int i = 0; i < h; ++i) {
		for (int ch = 0; ch < 4; ++ch) {
			int ti = i * w, li = ti, ri = ti + r;
			int fv = scl[ti * 4 + ch], lv = scl[(ti + w - 1) * 4 + ch], val = (r + 1) * fv;
			for (int j = 0; j < r; ++j) {
				val += scl[(ti + j) * 4 + ch];
			}
			for (int j = 0; j <= r; ++j) {
				val += scl[ri++ * 4 + ch] - fv;
				tcl[ti++ * 4 + ch] = round(val * iarr);
			}
			for (int j = r + 1; j < w - r; ++j) {
				val += scl[ri++ * 4 + ch] - scl[li++ * 4 + ch];
				tcl[ti++ * 4 + ch] = round(val * iarr);
			}
			for (int j = w - r; j < w; ++j) {
				val += lv - scl[li++ * 4 + ch];
				tcl[ti++ * 4 + ch] = round(val * iarr);
			}
		}
	}
}

void Blur::boxBlurT(unsigned char *scl, unsigned char *tcl, int w, int h, int r) {
	float iarr = 1.0 / (r + r + 1);

	#pragma omp parallel for shared (scl, tcl)
	for (int i = 0; i < w; i++) {
		for (int ch = 0; ch < 4; ++ch) {
			int ti = i, li = ti, ri = ti + r * w;
			int fv = scl[ti * 4 + ch], lv = scl[(ti + w * (h - 1)) * 4 + ch], val = (r + 1) * fv;
			for (int j = 0; j < r; j++) val += scl[(ti + j * w) * 4 + ch];
			for (int j = 0; j <= r; j++) {
				val += scl[ri * 4 + ch] - fv;
				tcl[ti * 4 + ch] = round(val * iarr);
				ri += w;
				ti += w;
			}
			for (int j = r + 1; j < h - r; j++) {
				val += scl[ri * 4 + ch] - scl[li * 4 + ch];
				tcl[ti * 4 + ch] = round(val * iarr);
				li += w;
				ri += w;
				ti += w;
			}
			for (int j = h - r; j < h; j++) {
				val += lv - scl[li * 4 + ch];
				tcl[ti * 4 + ch] = round(val * iarr);
				li += w;
				ti += w;
			}
		}
	}
}

// Generate JSON string of this object
std::string Blur::Json() const {

	// Return formatted string
	return JsonValue().toStyledString();
}

// Generate Json::Value for this object
Json::Value Blur::JsonValue() const {

	// Create root json object
	Json::Value root = EffectBase::JsonValue(); // get parent properties
	root["type"] = info.class_name;
	root["horizontal_radius"] = horizontal_radius.JsonValue();
	root["vertical_radius"] = vertical_radius.JsonValue();
	root["sigma"] = sigma.JsonValue();
	root["iterations"] = iterations.JsonValue();
	root["mask_mode"] = mask_mode;

	// return JsonValue
	return root;
}

// Load JSON string into this object
void Blur::SetJson(const std::string value) {

	// Parse JSON string into JSON objects
	try
	{
		const Json::Value root = openshot::stringToJson(value);
		// Set all values that match
		SetJsonValue(root);
	}
	catch (const std::exception& e)
	{
		// Error parsing JSON (or missing keys)
		throw InvalidJSON("JSON is invalid (missing keys or invalid data types)");
	}
}

// Load Json::Value into this object
void Blur::SetJsonValue(const Json::Value root) {

	// Set parent data
	EffectBase::SetJsonValue(root);

	// Set data from Json (if key is found)
	if (!root["horizontal_radius"].isNull())
		horizontal_radius.SetJsonValue(root["horizontal_radius"]);
	if (!root["vertical_radius"].isNull())
		vertical_radius.SetJsonValue(root["vertical_radius"]);
	if (!root["sigma"].isNull())
		sigma.SetJsonValue(root["sigma"]);
	if (!root["iterations"].isNull())
		iterations.SetJsonValue(root["iterations"]);
	if (!root["mask_mode"].isNull())
		mask_mode = root["mask_mode"].asInt();
}

// Get all properties for a specific frame
std::string Blur::PropertiesJSON(int64_t requested_frame) const {

	// Generate JSON properties list
	Json::Value root = BasePropertiesJSON(requested_frame);

	// Keyframes
	root["horizontal_radius"] = add_property_json("Horizontal Radius", horizontal_radius.GetValue(requested_frame), "float", "", &horizontal_radius, 0, 100, false, requested_frame);
	root["vertical_radius"] = add_property_json("Vertical Radius", vertical_radius.GetValue(requested_frame), "float", "", &vertical_radius, 0, 100, false, requested_frame);
	root["sigma"] = add_property_json("Sigma", sigma.GetValue(requested_frame), "float", "", &sigma, 0, 100, false, requested_frame);
	root["iterations"] = add_property_json("Iterations", iterations.GetValue(requested_frame), "float", "", &iterations, 0, 100, false, requested_frame);
	root["mask_mode"] = add_property_json("Mask Mode", mask_mode, "int", "", NULL, 0, 1, false, requested_frame);
	root["mask_mode"]["choices"].append(add_property_choice_json("Limit to Mask", BLUR_MASK_POST_BLEND, mask_mode));
	root["mask_mode"]["choices"].append(add_property_choice_json("Vary Strength", BLUR_MASK_DRIVE_AMOUNT, mask_mode));

	// Return formatted string
	return root.toStyledString();
}
