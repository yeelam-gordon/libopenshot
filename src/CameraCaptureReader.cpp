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

extern "C" {
	#include <libavdevice/avdevice.h>
}

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
#elif defined(_WIN32)
	return backend == CAMERA_CAPTURE_WINDOWS_DSHOW || backend == CAMERA_CAPTURE_AUTO;
#else
	(void) backend;
	return false;
#endif
}

CameraCaptureBackend CameraCaptureReader::DefaultBackend()
{
#if defined(__linux__)
	return CAMERA_CAPTURE_V4L2;
#elif defined(_WIN32)
	return CAMERA_CAPTURE_WINDOWS_DSHOW;
#else
	return CAMERA_CAPTURE_AUTO;
#endif
}

AudioDeviceList CameraCaptureReader::GetDeviceNames(CameraCaptureBackend backend)
{
	if (backend == CAMERA_CAPTURE_AUTO) {
		backend = DefaultBackend();
	}

	AudioDeviceList devices;
	const char* input_format_name = nullptr;
#if defined(__linux__)
	if (backend == CAMERA_CAPTURE_V4L2) {
		input_format_name = "v4l2";
	}
#elif defined(_WIN32)
	if (backend == CAMERA_CAPTURE_WINDOWS_DSHOW) {
		input_format_name = "dshow";
	}
#endif
	if (!input_format_name) {
		return devices;
	}

	avdevice_register_all();
	const AVInputFormat* input_format = av_find_input_format(input_format_name);
	if (!input_format) {
		return devices;
	}

	AVDeviceInfoList* device_list = nullptr;
	const int result = avdevice_list_input_sources(input_format, nullptr, nullptr, &device_list);
	if (result >= 0 && device_list) {
		for (int index = 0; index < device_list->nb_devices; ++index) {
			const AVDeviceInfo* device = device_list->devices[index];
			if (!device || !device->device_name) {
				continue;
			}
			const std::string name = device->device_name;
			const std::string label = device->device_description
				? device->device_description
				: device->device_name;
			devices.emplace_back(label, name);
		}
	}
	avdevice_free_list_devices(&device_list);
	return devices;
}

void CameraCaptureReader::ValidateSettings() const
{
	if (!IsBackendSupported(settings.backend)) {
		throw InvalidOptions("Camera capture backend is not supported on this OS.");
	}
	if (settings.backend != CAMERA_CAPTURE_V4L2 && settings.backend != CAMERA_CAPTURE_WINDOWS_DSHOW) {
		throw InvalidOptions("Camera capture backend is not implemented in this build.");
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
	converted.display = settings.device;
	converted.width = settings.width;
	converted.height = settings.height;
	converted.fps = settings.fps;
	converted.options = settings.options;
	if (settings.backend == CAMERA_CAPTURE_WINDOWS_DSHOW) {
		converted.backend = SCREEN_CAPTURE_WINDOWS_GDI;
		converted.display = "video=" + settings.device;
		converted.options["input_format_name"] = "dshow";
	} else {
		converted.backend = SCREEN_CAPTURE_X11;
		converted.options["input_format_name"] = "v4l2";
	}
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
