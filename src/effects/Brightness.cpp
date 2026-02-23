/**
 * @file
 * @brief Source file for Brightness class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2019 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "Brightness.h"
#include "Exceptions.h"

using namespace openshot;

/// Blank constructor, useful when using Json to load the effect properties
Brightness::Brightness() : brightness(0.0), contrast(3.0), mask_mode(BRIGHTNESS_MASK_LIMIT_TO_AREA) {
	// Init effect properties
	init_effect_details();
}

// Default constructor
Brightness::Brightness(Keyframe new_brightness, Keyframe new_contrast) :
	brightness(new_brightness), contrast(new_contrast), mask_mode(BRIGHTNESS_MASK_LIMIT_TO_AREA)
{
	// Init effect properties
	init_effect_details();
}

// Init effect settings
void Brightness::init_effect_details()
{
	/// Initialize the values of the EffectInfo struct.
	InitEffectInfo();

	/// Set the effect info
	info.class_name = "Brightness";
	info.name = "Brightness & Contrast";
	info.description = "Adjust the brightness and contrast of the frame's image.";
	info.has_audio = false;
	info.has_video = true;
}

// This method is required for all derived classes of EffectBase, and returns a
// modified openshot::Frame object
std::shared_ptr<openshot::Frame> Brightness::GetFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number)
{
	// Get the frame's image
	std::shared_ptr<QImage> frame_image = frame->GetImage();

	// Get keyframe values for this frame
	float brightness_value = brightness.GetValue(frame_number);
	float contrast_value = contrast.GetValue(frame_number);

	// Loop through pixels
	unsigned char *pixels = (unsigned char *) frame_image->bits();
	int pixel_count = frame_image->width() * frame_image->height();

	#pragma omp parallel for
	for (int pixel = 0; pixel < pixel_count; ++pixel)
	{
		// Compute contrast adjustment factor
		float factor = (259 * (contrast_value + 255)) / (255 * (259 - contrast_value));

		// Calculate alpha % (to be used for removing pre-multiplied alpha value)
		int A = pixels[pixel * 4 + 3];
		float alpha_percent = A / 255.0;

		// Get RGB values, and remove pre-multiplied alpha
		unsigned char R = pixels[pixel * 4 + 0] / alpha_percent;
		unsigned char G = pixels[pixel * 4 + 1] / alpha_percent;
		unsigned char B = pixels[pixel * 4 + 2] / alpha_percent;

		// Apply constrained contrast adjustment
		R = constrain((factor * (R - 128)) + 128);
		G = constrain((factor * (G - 128)) + 128);
		B = constrain((factor * (B - 128)) + 128);

		// Adjust brightness and write constrained values back to image
		pixels[pixel * 4 + 0] = constrain(R + (255 * brightness_value));
		pixels[pixel * 4 + 1] = constrain(G + (255 * brightness_value));
		pixels[pixel * 4 + 2] = constrain(B + (255 * brightness_value));

		// Pre-multiply the alpha back into the color channels
		pixels[pixel * 4 + 0] *= alpha_percent;
		pixels[pixel * 4 + 1] *= alpha_percent;
		pixels[pixel * 4 + 2] *= alpha_percent;
	}

	// return the modified frame
	return frame;
}

bool Brightness::UseCustomMaskBlend(int64_t frame_number) const {
	(void) frame_number;
	return mask_mode == BRIGHTNESS_MASK_VARY_STRENGTH;
}

void Brightness::ApplyCustomMaskBlend(std::shared_ptr<QImage> original_image, std::shared_ptr<QImage> effected_image,
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
		factor = factor * factor;
		const float inverse = 1.0f - factor;

		effected_pixels[idx] = static_cast<unsigned char>(
			(original_pixels[idx] * inverse) + (effected_pixels[idx] * factor));
		effected_pixels[idx + 1] = static_cast<unsigned char>(
			(original_pixels[idx + 1] * inverse) + (effected_pixels[idx + 1] * factor));
		effected_pixels[idx + 2] = static_cast<unsigned char>(
			(original_pixels[idx + 2] * inverse) + (effected_pixels[idx + 2] * factor));
		effected_pixels[idx + 3] = original_pixels[idx + 3];
	}
}

// Generate JSON string of this object
std::string Brightness::Json() const {

	// Return formatted string
	return JsonValue().toStyledString();
}

// Generate Json::Value for this object
Json::Value Brightness::JsonValue() const {

	// Create root json object
	Json::Value root = EffectBase::JsonValue(); // get parent properties
	root["type"] = info.class_name;
	root["brightness"] = brightness.JsonValue();
	root["contrast"] = contrast.JsonValue();
	root["mask_mode"] = mask_mode;

	// return JsonValue
	return root;
}

// Load JSON string into this object
void Brightness::SetJson(const std::string value) {

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
void Brightness::SetJsonValue(const Json::Value root) {

	// Set parent data
	EffectBase::SetJsonValue(root);

	// Set data from Json (if key is found)
	if (!root["brightness"].isNull())
		brightness.SetJsonValue(root["brightness"]);
	if (!root["contrast"].isNull())
		contrast.SetJsonValue(root["contrast"]);
	if (!root["mask_mode"].isNull())
		mask_mode = root["mask_mode"].asInt();
}

// Get all properties for a specific frame
std::string Brightness::PropertiesJSON(int64_t requested_frame) const {

	// Generate JSON properties list
	Json::Value root = BasePropertiesJSON(requested_frame);

	// Keyframes
	root["brightness"] = add_property_json("Brightness", brightness.GetValue(requested_frame), "float", "", &brightness, -1.0, 1.0, false, requested_frame);
	root["contrast"] = add_property_json("Contrast", contrast.GetValue(requested_frame), "float", "", &contrast, -128, 128.0, false, requested_frame);
	root["mask_mode"] = add_property_json("Mask Mode", mask_mode, "int", "", NULL, 0, 1, false, requested_frame);
	root["mask_mode"]["choices"].append(add_property_choice_json("Limit to Mask", BRIGHTNESS_MASK_LIMIT_TO_AREA, mask_mode));
	root["mask_mode"]["choices"].append(add_property_choice_json("Vary Strength", BRIGHTNESS_MASK_VARY_STRENGTH, mask_mode));

	// Return formatted string
	return root.toStyledString();
}
