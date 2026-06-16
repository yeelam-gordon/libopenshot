/**
 * @file
 * @brief Source file for live FFmpeg camera capture readers
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "CameraCaptureReader.h"

#include <cstdlib>

#include "Exceptions.h"

using namespace openshot;

CameraCaptureReader::CameraCaptureReader(const CameraCaptureSettings& new_settings)
	: settings(new_settings)
	, reader(nullptr)
{
	if (settings.backend == CAMERA_CAPTURE_AUTO) {
		settings.backend = DefaultBackend();
	}
	ValidateSettings();
	RebuildReader();
}

CameraCaptureReader::~CameraCaptureReader()
{
	Close();
}

bool CameraCaptureReader::IsBackendSupported(CameraCaptureBackend backend)
{
#if defined(__linux__)
	return backend == CAMERA_CAPTURE_V4L2 || backend == CAMERA_CAPTURE_AUTO;
#else
	(void) backend;
	return false;
#endif
}

CameraCaptureBackend CameraCaptureReader::DefaultBackend()
{
#if defined(__linux__)
	return CAMERA_CAPTURE_V4L2;
#else
	return CAMERA_CAPTURE_AUTO;
#endif
}

void CameraCaptureReader::ValidateSettings() const
{
	if (!IsBackendSupported(settings.backend)) {
		throw InvalidOptions("Camera capture backend is not supported on this OS.");
	}
	if (settings.backend != CAMERA_CAPTURE_V4L2) {
		throw InvalidOptions("Only the v4l2 camera capture backend is implemented in this build.");
	}
	if (settings.device.empty()) {
		throw InvalidOptions("Camera capture requires a device path.");
	}
	if (settings.width <= 0 || settings.height <= 0) {
		throw InvalidOptions("Camera capture requires a positive width and height.");
	}
	if (settings.fps.num <= 0 || settings.fps.den <= 0) {
		throw InvalidOptions("Camera capture requires a positive frame rate.");
	}
}

ScreenCaptureSettings CameraCaptureReader::ToDeviceSettings() const
{
	ScreenCaptureSettings converted;
	converted.backend = SCREEN_CAPTURE_X11;
	converted.display = settings.device;
	converted.width = settings.width;
	converted.height = settings.height;
	converted.fps = settings.fps;
	converted.options = settings.options;
	converted.options["input_format_name"] = "v4l2";
	return converted;
}

void CameraCaptureReader::RebuildReader()
{
	reader = std::make_unique<ScreenCaptureReader>(ToDeviceSettings());
	info = reader->info;
	info.metadata["capture_type"] = "camera";
}

void CameraCaptureReader::Open()
{
	reader->Open();
	info = reader->info;
	info.metadata["capture_type"] = "camera";
}

void CameraCaptureReader::Close()
{
	if (reader) {
		reader->Close();
	}
}

openshot::CaptureReaderStats CameraCaptureReader::GetStats() const
{
	return reader ? reader->GetStats() : CaptureReaderStats();
}

std::shared_ptr<Frame> CameraCaptureReader::GetFrame(int64_t number)
{
	return reader->GetFrame(number);
}

std::string CameraCaptureReader::Json() const
{
	return JsonValue().toStyledString();
}

Json::Value CameraCaptureReader::JsonValue() const
{
	Json::Value root = ReaderBase::JsonValue();
	root["type"] = "CameraCaptureReader";
	root["backend"] = settings.backend;
	root["device"] = settings.device;
	root["width"] = settings.width;
	root["height"] = settings.height;
	root["fps"]["num"] = settings.fps.num;
	root["fps"]["den"] = settings.fps.den;
	root["options"] = Json::Value(Json::objectValue);
	for (const auto& option : settings.options) {
		root["options"][option.first] = option.second;
	}
	return root;
}

void CameraCaptureReader::SetJson(const std::string value)
{
	try {
		SetJsonValue(openshot::stringToJson(value));
	} catch (const std::exception&) {
		throw InvalidJSON("JSON is invalid (missing keys or invalid data types)");
	}
}

void CameraCaptureReader::SetJsonValue(const Json::Value root)
{
	if (!root["backend"].isNull())
		settings.backend = static_cast<CameraCaptureBackend>(root["backend"].asInt());
	if (!root["device"].isNull())
		settings.device = root["device"].asString();
	if (!root["width"].isNull())
		settings.width = root["width"].asInt();
	if (!root["height"].isNull())
		settings.height = root["height"].asInt();
	if (!root["fps"].isNull() && root["fps"].isObject()) {
		if (!root["fps"]["num"].isNull())
			settings.fps.num = root["fps"]["num"].asInt();
		if (!root["fps"]["den"].isNull())
			settings.fps.den = root["fps"]["den"].asInt();
	}
	if (!root["options"].isNull() && root["options"].isObject()) {
		settings.options.clear();
		for (const auto& key : root["options"].getMemberNames()) {
			settings.options[key] = root["options"][key].asString();
		}
	}
	if (settings.backend == CAMERA_CAPTURE_AUTO) {
		settings.backend = DefaultBackend();
	}
	ValidateSettings();
	RebuildReader();
}
