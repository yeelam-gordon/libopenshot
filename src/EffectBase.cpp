/**
 * @file
 * @brief Source file for EffectBase class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2019 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <iostream>
#include <iomanip>
#include <algorithm>

#include "EffectBase.h"

#include "Exceptions.h"
#include "Timeline.h"
#include "ReaderBase.h"
#include "ChunkReader.h"
#include "FFmpegReader.h"
#include "QtImageReader.h"
#include <omp.h>

#ifdef USE_IMAGEMAGICK
	#include "ImageReader.h"
#endif

using namespace openshot;

// Initialize the values of the EffectInfo struct
void EffectBase::InitEffectInfo()
{
	// Init clip settings
	Position(0.0);
	Layer(0);
	Start(0.0);
	End(0.0);
	Order(0);
	ParentClip(NULL);
	parentEffect = NULL;
	mask_invert = false;
	mask_reader = NULL;

	info.has_video = false;
	info.has_audio = false;
	info.has_tracked_object = false;
	info.name = "";
	info.description = "";
	info.parent_effect_id = "";
	info.apply_before_clip = true;
}

// Display file information
void EffectBase::DisplayInfo(std::ostream* out) {
	*out << std::fixed << std::setprecision(2) << std::boolalpha;
	*out << "----------------------------" << std::endl;
	*out << "----- Effect Information -----" << std::endl;
	*out << "----------------------------" << std::endl;
	*out << "--> Name: " << info.name << std::endl;
	*out << "--> Description: " << info.description << std::endl;
	*out << "--> Has Video: " << info.has_video << std::endl;
	*out << "--> Has Audio: " << info.has_audio << std::endl;
	*out << "--> Apply Before Clip Keyframes: " << info.apply_before_clip << std::endl;
	*out << "--> Order: " << order << std::endl;
	*out << "----------------------------" << std::endl;
}

// Constrain a color value from 0 to 255
int EffectBase::constrain(int color_value)
{
	// Constrain new color from 0 to 255
	if (color_value < 0)
		color_value = 0;
	else if (color_value > 255)
		color_value = 255;

	return color_value;
}

// Generate JSON string of this object
std::string EffectBase::Json() const {

	// Return formatted string
	return JsonValue().toStyledString();
}

// Generate Json::Value for this object
Json::Value EffectBase::JsonValue() const {

	// Create root json object
	Json::Value root = ClipBase::JsonValue(); // get parent properties
	root["name"] = info.name;
	root["class_name"] = info.class_name;
	root["description"] = info.description;
	root["parent_effect_id"] = info.parent_effect_id;
	root["has_video"] = info.has_video;
	root["has_audio"] = info.has_audio;
	root["has_tracked_object"] = info.has_tracked_object;
	root["apply_before_clip"] = info.apply_before_clip;
	root["order"] = Order();
	root["mask_invert"] = mask_invert;
	if (mask_reader)
		root["mask_reader"] = mask_reader->JsonValue();
	else
		root["mask_reader"] = Json::objectValue;

	// return JsonValue
	return root;
}

// Load JSON string into this object
void EffectBase::SetJson(const std::string value) {

	// Parse JSON string into JSON objects
	try
	{
		Json::Value root = openshot::stringToJson(value);
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
void EffectBase::SetJsonValue(const Json::Value root) {

	if (ParentTimeline()){
		// Get parent timeline
		Timeline* parentTimeline = static_cast<Timeline *>(ParentTimeline());

		// Get the list of effects on the timeline
		std::list<EffectBase*> effects = parentTimeline->ClipEffects();

		// TODO: Fix recursive call for Object Detection

		// // Loop through the effects and check if we have a child effect linked to this effect
		for (auto const& effect : effects){
			// Set the properties of all effects which parentEffect points to this
			if ((effect->info.parent_effect_id == this->Id()) && (effect->Id() != this->Id()))
				effect->SetJsonValue(root);
		}
	}

	// Set this effect properties with the parent effect properties (except the id and parent_effect_id)
	Json::Value my_root;
	if (parentEffect){
		my_root = parentEffect->JsonValue();
		my_root["id"] = this->Id();
		my_root["parent_effect_id"] = this->info.parent_effect_id;
	} else {
		my_root = root;
	}

	// Set parent data
	ClipBase::SetJsonValue(my_root);

	// Set data from Json (if key is found)
	if (!my_root["order"].isNull())
		Order(my_root["order"].asInt());

	if (!my_root["apply_before_clip"].isNull())
		info.apply_before_clip = my_root["apply_before_clip"].asBool();

	if (!my_root["mask_invert"].isNull())
		mask_invert = my_root["mask_invert"].asBool();

	const Json::Value mask_reader_json =
		!my_root["mask_reader"].isNull() ? my_root["mask_reader"] : my_root["reader"];

	if (!mask_reader_json.isNull()) {
		if (!mask_reader_json["type"].isNull()) {
			MaskReader(CreateReaderFromJson(mask_reader_json));
		} else if (mask_reader_json.isObject() && mask_reader_json.empty()) {
			MaskReader(NULL);
		}
	}

	if (!my_root["parent_effect_id"].isNull()){
		info.parent_effect_id = my_root["parent_effect_id"].asString();
		if (info.parent_effect_id.size() > 0 && info.parent_effect_id != "" && parentEffect == NULL)
			SetParentEffect(info.parent_effect_id);
		else
			parentEffect = NULL;
	}
}

// Generate Json::Value for this object
Json::Value EffectBase::JsonInfo() const {

	// Create root json object
	Json::Value root;
	root["name"] = info.name;
	root["class_name"] = info.class_name;
	root["description"] = info.description;
	root["has_video"] = info.has_video;
	root["has_audio"] = info.has_audio;

	// return JsonValue
	return root;
}

// Get all properties for a specific frame
Json::Value EffectBase::BasePropertiesJSON(int64_t requested_frame) const {
	// Generate JSON properties list
	Json::Value root;
	root["id"] = add_property_json("ID", 0.0, "string", Id(), NULL, -1, -1, true, requested_frame);
	root["position"] = add_property_json("Position", Position(), "float", "", NULL, 0, 30 * 60 * 60 * 48, false, requested_frame);
	root["layer"] = add_property_json("Track", Layer(), "int", "", NULL, 0, 20, false, requested_frame);
	root["start"] = add_property_json("Start", Start(), "float", "", NULL, 0, 30 * 60 * 60 * 48, false, requested_frame);
	root["end"] = add_property_json("End", End(), "float", "", NULL, 0, 30 * 60 * 60 * 48, false, requested_frame);
	root["duration"] = add_property_json("Duration", Duration(), "float", "", NULL, 0, 30 * 60 * 60 * 48, true, requested_frame);

	// Add replace_image choices (dropdown style)
	root["apply_before_clip"] = add_property_json("Apply Before Clip Keyframes", info.apply_before_clip, "int", "", NULL, 0, 1, false, requested_frame);
	root["apply_before_clip"]["choices"].append(add_property_choice_json("Yes", true, info.apply_before_clip));
	root["apply_before_clip"]["choices"].append(add_property_choice_json("No", false, info.apply_before_clip));

	// Set the parent effect which properties this effect will inherit
	root["parent_effect_id"] = add_property_json("Parent", 0.0, "string", info.parent_effect_id, NULL, -1, -1, false, requested_frame);

	if (info.has_video) {
		root["mask_invert"] = add_property_json("Invert Mask", mask_invert, "int", "", NULL, 0, 1, false, requested_frame);
		root["mask_invert"]["choices"].append(add_property_choice_json("Yes", true, mask_invert));
		root["mask_invert"]["choices"].append(add_property_choice_json("No", false, mask_invert));

		if (mask_reader)
			root["mask_reader"] = add_property_json("Mask Source", 0.0, "reader", mask_reader->Json(), NULL, 0, 1, false, requested_frame);
		else
			root["mask_reader"] = add_property_json("Mask Source", 0.0, "reader", "{}", NULL, 0, 1, false, requested_frame);
	}

	return root;
}

ReaderBase* EffectBase::CreateReaderFromJson(const Json::Value& reader_json) const {
	if (reader_json["type"].isNull())
		return NULL;

	ReaderBase* reader = NULL;
	const std::string type = reader_json["type"].asString();

	if (type == "FFmpegReader") {
		reader = new FFmpegReader(reader_json["path"].asString());
		reader->SetJsonValue(reader_json);
		// Mask readers are video-only sources. Disabling audio avoids FFmpeg
		// A/V readiness fallbacks that can repeat stale video frames.
		reader->info.has_audio = false;
		reader->info.audio_stream_index = -1;
	} else if (type == "QtImageReader") {
		reader = new QtImageReader(reader_json["path"].asString());
		reader->SetJsonValue(reader_json);
	} else if (type == "ChunkReader") {
		reader = new ChunkReader(reader_json["path"].asString(),
								 static_cast<ChunkVersion>(reader_json["chunk_version"].asInt()));
		reader->SetJsonValue(reader_json);
#ifdef USE_IMAGEMAGICK
	} else if (type == "ImageReader") {
		reader = new ImageReader(reader_json["path"].asString());
		reader->SetJsonValue(reader_json);
#endif
	}

	return reader;
}

void EffectBase::MaskReader(ReaderBase* new_reader) {
	if (mask_reader == new_reader)
		return;

	if (mask_reader) {
		mask_reader->Close();
		delete mask_reader;
	}

	mask_reader = new_reader;
	if (mask_reader)
		mask_reader->ParentClip(clip);
}

std::shared_ptr<QImage> EffectBase::GetMaskImage(std::shared_ptr<QImage> target_image, int64_t frame_number) {
	if (!mask_reader || !target_image || target_image->isNull())
		return {};

	std::shared_ptr<QImage> source_mask;
	#pragma omp critical (open_effect_mask_reader)
	{
		if (!mask_reader->IsOpen())
			mask_reader->Open();
		auto source_frame = mask_reader->GetFrame(frame_number);
		if (source_frame && source_frame->GetImage() && !source_frame->GetImage()->isNull())
			source_mask = std::make_shared<QImage>(*source_frame->GetImage());
	}

	if (!source_mask || source_mask->isNull())
		return {};

	return std::make_shared<QImage>(
		source_mask->scaled(
			target_image->width(), target_image->height(),
			Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void EffectBase::BlendWithMask(std::shared_ptr<QImage> original_image, std::shared_ptr<QImage> effected_image,
							   std::shared_ptr<QImage> mask_image) const {
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
		int gray = qGray(mask_pixels[idx], mask_pixels[idx + 1], mask_pixels[idx + 2]);
		if (mask_invert)
			gray = 255 - gray;
		const float factor = static_cast<float>(gray) / 255.0f;
		const float inverse = 1.0f - factor;

		effected_pixels[idx] = static_cast<unsigned char>(
			(original_pixels[idx] * inverse) + (effected_pixels[idx] * factor));
		effected_pixels[idx + 1] = static_cast<unsigned char>(
			(original_pixels[idx + 1] * inverse) + (effected_pixels[idx + 1] * factor));
		effected_pixels[idx + 2] = static_cast<unsigned char>(
			(original_pixels[idx + 2] * inverse) + (effected_pixels[idx + 2] * factor));
		effected_pixels[idx + 3] = static_cast<unsigned char>(
			(original_pixels[idx + 3] * inverse) + (effected_pixels[idx + 3] * factor));
	}
}

std::shared_ptr<openshot::Frame> EffectBase::ProcessFrame(std::shared_ptr<openshot::Frame> frame, int64_t frame_number) {
	// Audio-only effects skip common mask handling.
	if (!info.has_video || !mask_reader)
		return GetFrame(frame, frame_number);

	auto pre_image = frame->GetImage();
	if (!pre_image || pre_image->isNull())
		return GetFrame(frame, frame_number);

	const auto original_image = std::make_shared<QImage>(pre_image->copy());
	auto output_frame = GetFrame(frame, frame_number);
	if (!output_frame)
		return output_frame;
	auto effected_image = output_frame->GetImage();
	if (!effected_image || effected_image->isNull() ||
		effected_image->size() != original_image->size())
		return output_frame;

	auto mask_image = GetMaskImage(effected_image, frame_number);
	if (!mask_image || mask_image->isNull())
		return output_frame;

	if (UseCustomMaskBlend(frame_number))
		ApplyCustomMaskBlend(original_image, effected_image, mask_image, frame_number);
	else
		BlendWithMask(original_image, effected_image, mask_image);

	return output_frame;
}

/// Parent clip object of this reader (which can be unparented and NULL)
openshot::ClipBase* EffectBase::ParentClip() {
	return clip;
}

/// Set parent clip object of this reader
void EffectBase::ParentClip(openshot::ClipBase* new_clip) {
	clip = new_clip;
	if (mask_reader)
		mask_reader->ParentClip(new_clip);
}

// Set the parent effect from which this properties will be set to
void EffectBase::SetParentEffect(std::string parentEffect_id) {

	// Get parent Timeline
	Timeline* parentTimeline = static_cast<Timeline *>(ParentTimeline());

	if (parentTimeline){

		// Get a pointer to the parentEffect
		EffectBase* parentEffectPtr = parentTimeline->GetClipEffect(parentEffect_id);

		if (parentEffectPtr){
			// Set the parent Effect
			parentEffect = parentEffectPtr;

			// Set the properties of this effect with the parent effect's properties
			Json::Value EffectJSON = parentEffect->JsonValue();
			EffectJSON["id"] = this->Id();
			EffectJSON["parent_effect_id"] = this->info.parent_effect_id;
			this->SetJsonValue(EffectJSON);
		}
	}
	return;
}

// Return the ID of this effect's parent clip
std::string EffectBase::ParentClipId() const{
	if(clip)
		return clip->Id();
	else
		return "";
}

EffectBase::~EffectBase() {
	MaskReader(NULL);
}
