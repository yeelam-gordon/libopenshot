/**
 * @file
 * @brief Source file for Mask class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2019 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "Mask.h"

#include "Exceptions.h"

#include "ReaderBase.h"
#include <omp.h>

using namespace openshot;

/// Blank constructor, useful when using Json to load the effect properties
Mask::Mask() : replace_image(false) {
	// Init effect properties
	init_effect_details();
}

// Default constructor
Mask::Mask(ReaderBase *mask_reader, Keyframe mask_brightness, Keyframe mask_contrast) :
		brightness(mask_brightness), contrast(mask_contrast), replace_image(false)
{
	// Init effect properties
	init_effect_details();

	// Keep ownership local by cloning externally-provided readers.
	if (mask_reader)
		MaskReader(CreateReaderFromJson(mask_reader->JsonValue()));
}

// Init effect settings
void Mask::init_effect_details()
{
	/// Initialize the values of the EffectInfo struct.
	InitEffectInfo();

	/// Set the effect info
	info.class_name = "Mask";
	info.name = "Alpha Mask / Wipe Transition";
	info.description = "Uses a grayscale mask image to gradually wipe / transition between 2 images.";
	info.has_audio = false;
	info.has_video = true;
}

// This method is required for all derived classes of EffectBase, and returns a
// modified openshot::Frame object
std::shared_ptr<openshot::Frame> Mask::GetFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number) {
	// Get frame image first
	std::shared_ptr<QImage> frame_image = frame->GetImage();
	if (!frame_image || frame_image->isNull())
		return frame;

	// No reader (bail on applying the mask)
	auto original_mask = ResolveMaskImage(frame_image, frame_number);
	if (!original_mask || original_mask->isNull())
		return frame;

	// Grab raw pointers and dimensions one time
	unsigned char* pixels      = reinterpret_cast<unsigned char*>(frame_image->bits());
	unsigned char* mask_pixels = reinterpret_cast<unsigned char*>(original_mask->bits());
	int width                   = original_mask->width();
	int height                  = original_mask->height();
	int num_pixels              = width * height;  // total pixel count

	// Evaluate brightness and contrast keyframes just once
	double contrast_value   = contrast.GetValue(frame_number);
	double brightness_value = brightness.GetValue(frame_number);

	int brightness_adj = static_cast<int>(255 * brightness_value);
	float contrast_factor = 20.0f / std::max(0.00001f, 20.0f - static_cast<float>(contrast_value));

	// Iterate over every pixel in parallel
#pragma omp parallel for schedule(static)
	for (int i = 0; i < num_pixels; ++i)
	{
		int idx = i * 4;

		int R = mask_pixels[idx + 0];
		int G = mask_pixels[idx + 1];
		int B = mask_pixels[idx + 2];
		int A = mask_pixels[idx + 3];

		// Compute base gray, then apply brightness + contrast
		int gray = qGray(R, G, B);
		gray += brightness_adj;
		gray = static_cast<int>(contrast_factor * (gray - 128) + 128);

		// Clamp (A - gray) into [0, 255]
		int diff = A - gray;
		if (diff < 0) diff = 0;
		else if (diff > 255) diff = 255;

		// Calculate the % change in alpha
		float alpha_percent = static_cast<float>(diff) / 255.0f;

		// Set the alpha channel to the gray value
		if (replace_image) {
			// Replace frame pixels with gray value (including alpha channel)
			auto new_val = static_cast<unsigned char>(diff);
			pixels[idx + 0] = new_val;
			pixels[idx + 1] = new_val;
			pixels[idx + 2] = new_val;
			pixels[idx + 3] = new_val;
		} else {
			// Premultiplied RGBA → multiply each channel by alpha_percent
			pixels[idx + 0] = static_cast<unsigned char>(pixels[idx + 0] * alpha_percent);
			pixels[idx + 1] = static_cast<unsigned char>(pixels[idx + 1] * alpha_percent);
			pixels[idx + 2] = static_cast<unsigned char>(pixels[idx + 2] * alpha_percent);
			pixels[idx + 3] = static_cast<unsigned char>(pixels[idx + 3] * alpha_percent);
		}

	}

	// return the modified frame
	return frame;
}

bool Mask::UseCustomMaskBlend(int64_t frame_number) const {
	(void) frame_number;
	// Mask effect already applies its own mask operation in GetFrame().
	return true;
}

void Mask::ApplyCustomMaskBlend(std::shared_ptr<QImage> original_image, std::shared_ptr<QImage> effected_image,
								std::shared_ptr<QImage> mask_image, int64_t frame_number) const {
	(void) original_image;
	(void) effected_image;
	(void) mask_image;
	(void) frame_number;
	// Intentionally no-op to skip base post-blend for Mask effects.
}

// Generate JSON string of this object
std::string Mask::Json() const {

	// Return formatted string
	return JsonValue().toStyledString();
}

// Generate Json::Value for this object
Json::Value Mask::JsonValue() const {

	// Create root json object
	Json::Value root = EffectBase::JsonValue(); // get parent properties
	root["type"] = info.class_name;
	root["brightness"] = brightness.JsonValue();
	root["contrast"] = contrast.JsonValue();
	root["replace_image"] = replace_image;

	// return JsonValue
	return root;
}

// Load JSON string into this object
void Mask::SetJson(const std::string value) {

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
void Mask::SetJsonValue(const Json::Value root) {
	Json::Value normalized_root = root;
	// Legacy compatibility: keep accepting "reader" on Mask effects.
	if (!normalized_root["reader"].isNull() && normalized_root["mask_reader"].isNull())
		normalized_root["mask_reader"] = normalized_root["reader"];

	// Set parent data
	EffectBase::SetJsonValue(normalized_root);

	// Set data from Json (if key is found)
	if (!normalized_root["replace_image"].isNull())
		replace_image = normalized_root["replace_image"].asBool();
	if (!normalized_root["brightness"].isNull())
		brightness.SetJsonValue(normalized_root["brightness"]);
	if (!normalized_root["contrast"].isNull())
		contrast.SetJsonValue(normalized_root["contrast"]);

}

// Get all properties for a specific frame
std::string Mask::PropertiesJSON(int64_t requested_frame) const {

	// Generate JSON properties list
	Json::Value root = BasePropertiesJSON(requested_frame);

	// Add replace_image choices (dropdown style)
	root["replace_image"] = add_property_json("Replace Image", replace_image, "int", "", NULL, 0, 1, false, requested_frame);
	root["replace_image"]["choices"].append(add_property_choice_json("Yes", true, replace_image));
	root["replace_image"]["choices"].append(add_property_choice_json("No", false, replace_image));

	// Keyframes
	root["brightness"] = add_property_json("Brightness", brightness.GetValue(requested_frame), "float", "", &brightness, -1.0, 1.0, false, requested_frame);
	root["contrast"] = add_property_json("Contrast", contrast.GetValue(requested_frame), "float", "", &contrast, 0, 20, false, requested_frame);

	// Return formatted string
	return root.toStyledString();
}

void Mask::Reader(ReaderBase *new_reader) {
	if (!new_reader) {
		MaskReader(NULL);
		return;
	}

	// Keep ownership local by cloning externally-provided readers.
	MaskReader(CreateReaderFromJson(new_reader->JsonValue()));
}
