/**
 * @file
 * @brief Source file for live FFmpeg screen capture readers
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ScreenCaptureReader.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

extern "C" {
	#include <libavdevice/avdevice.h>
	#include <libavutil/imgutils.h>
}

#include "Exceptions.h"
#include "Frame.h"

using namespace openshot;

namespace
{
	std::string fraction_to_string(const Fraction& value)
	{
		std::stringstream stream;
		stream << value.num << "/" << value.den;
		return stream.str();
	}

	void set_option(AVDictionary** options, const std::string& key, const std::string& value)
	{
		av_dict_set(options, key.c_str(), value.c_str(), 0);
	}
}

#if defined(HAVE_WAYLAND_CAPTURE)
std::unique_ptr<ScreenCaptureReader::CaptureBackendReader> CreateWaylandScreenCaptureReader(
	const ScreenCaptureSettings& settings,
	ReaderInfo& info);
#endif

ScreenCaptureReader::ScreenCaptureReader(const ScreenCaptureSettings& new_settings)
	: settings(new_settings)
	, backend_reader(nullptr)
	, is_open(false)
	, video_stream(-1)
	, frames_read(0)
	, dropped_packets(0)
	, format_context(nullptr)
	, codec_context(nullptr)
	, source_frame(nullptr)
	, rgba_frame(nullptr)
	, packet(nullptr)
	, sws_context(nullptr)
{
	if (settings.backend == SCREEN_CAPTURE_AUTO) {
		settings.backend = DefaultBackend();
	}
	ValidateSettings();
	PopulateInfo();
	if (UsesWaylandPortal()) {
	#if defined(HAVE_WAYLAND_CAPTURE)
		backend_reader = CreateWaylandScreenCaptureReader(settings, info);
		if (!backend_reader) {
			throw InvalidOptions("Wayland screen capture backend is unavailable in this build.");
		}
	#else
		throw InvalidOptions("Wayland screen capture backend is unavailable in this build.");
	#endif
	}
}

ScreenCaptureReader::~ScreenCaptureReader()
{
	Close();
}

bool ScreenCaptureReader::IsOpen()
{
	return backend_reader ? backend_reader->IsOpen() : is_open;
}

bool ScreenCaptureReader::IsBackendSupported(ScreenCaptureBackend backend)
{
#if defined(__linux__)
	if (backend == SCREEN_CAPTURE_X11 || backend == SCREEN_CAPTURE_AUTO) {
		return true;
	}
#if defined(HAVE_WAYLAND_CAPTURE)
	if (backend == SCREEN_CAPTURE_WAYLAND) {
		return true;
	}
#endif
	return false;
#else
	(void) backend;
	return false;
#endif
}

ScreenCaptureBackend ScreenCaptureReader::DefaultBackend()
{
#if defined(__linux__)
	const char* session = std::getenv("XDG_SESSION_TYPE");
	if (session && std::string(session) == "wayland" && IsBackendSupported(SCREEN_CAPTURE_WAYLAND)) {
		return SCREEN_CAPTURE_WAYLAND;
	}
	if (!session || std::string(session) == "x11") {
		return SCREEN_CAPTURE_X11;
	}
#endif
	return SCREEN_CAPTURE_AUTO;
}

void ScreenCaptureReader::ValidateSettings() const
{
	const bool using_v4l2_device = settings.options.count("input_format_name") && settings.options.at("input_format_name") == "v4l2";
	if (!IsBackendSupported(settings.backend)) {
		throw InvalidOptions("Screen capture backend is not supported on this OS or session.");
	}
	if (!UsesFFmpegDevice() && !UsesWaylandPortal() && !using_v4l2_device) {
		throw InvalidOptions("Only the X11 screen capture backend is implemented in this build.");
	}
	if (settings.width <= 0 || settings.height <= 0) {
		throw InvalidOptions("Screen capture requires a positive width and height.");
	}
	if (settings.fps.num <= 0 || settings.fps.den <= 0) {
		throw InvalidOptions("Screen capture requires a positive frame rate.");
	}
}

bool ScreenCaptureReader::UsesFFmpegDevice() const
{
	const bool using_v4l2_device = settings.options.count("input_format_name") && settings.options.at("input_format_name") == "v4l2";
	return settings.backend == SCREEN_CAPTURE_X11 || using_v4l2_device;
}

bool ScreenCaptureReader::UsesWaylandPortal() const
{
	return settings.backend == SCREEN_CAPTURE_WAYLAND;
}

std::string ScreenCaptureReader::InputFormatName() const
{
	const auto override_format = settings.options.find("input_format_name");
	if (override_format != settings.options.end()) {
		return override_format->second;
	}
	if (settings.backend == SCREEN_CAPTURE_X11) {
		return "x11grab";
	}
	return "";
}

std::string ScreenCaptureReader::InputName() const
{
	if (InputFormatName() == "v4l2") {
		return settings.display.empty() ? "/dev/video0" : settings.display;
	}

	std::string display = settings.display;
	if (display.empty()) {
		const char* env_display = std::getenv("DISPLAY");
		display = env_display ? env_display : ":0.0";
	}
	if (InputFormatName() == "x11grab" && settings.options.count("window_id")) {
		return display;
	}

	std::stringstream input;
	input << display << "+" << settings.x << "," << settings.y;
	return input.str();
}

void ScreenCaptureReader::PopulateInfo()
{
	info.has_video = true;
	info.has_audio = false;
	info.has_single_image = false;
	info.duration = 60.0f * 60.0f;
	info.file_size = 0;
	info.width = settings.width;
	info.height = settings.height;
	info.pixel_format = AV_PIX_FMT_RGBA;
	info.fps = settings.fps;
	info.video_bit_rate = 0;
	info.pixel_ratio = Fraction(1, 1);
	info.display_ratio = Fraction(settings.width, settings.height);
	info.display_ratio.Reduce();
	info.vcodec = "rawvideo";
	info.video_length = static_cast<int64_t>(info.duration * settings.fps.ToFloat());
	info.video_stream_index = -1;
	info.video_timebase = settings.fps.Reciprocal();
	info.interlaced_frame = false;
	info.top_field_first = false;
	info.acodec = "";
	info.audio_bit_rate = 0;
	info.sample_rate = 0;
	info.channels = 0;
	info.channel_layout = LAYOUT_MONO;
	info.audio_stream_index = -1;
	info.audio_timebase = Fraction(1, 1);
}

void ScreenCaptureReader::Open()
{
	if (backend_reader) {
		backend_reader->Open();
		return;
	}
	if (is_open) {
		return;
	}
	OpenDevice();
	OpenDecoder();
	is_open = true;
}

void ScreenCaptureReader::OpenDevice()
{
	avdevice_register_all();

	const AVInputFormat* input_format = av_find_input_format(InputFormatName().c_str());
	if (!input_format) {
		throw InvalidOptions("FFmpeg input device is not available: " + InputFormatName(), InputName());
	}

	AVDictionary* options = nullptr;
	set_option(&options, "framerate", fraction_to_string(settings.fps));
	set_option(&options, "video_size", std::to_string(settings.width) + "x" + std::to_string(settings.height));
	if (InputFormatName() == "x11grab") {
		set_option(&options, "draw_mouse", settings.include_cursor ? "1" : "0");
		set_option(&options, "show_region", settings.show_region ? "1" : "0");
	}
	for (const auto& option : settings.options) {
		if (option.first == "input_format_name") {
			continue;
		}
		set_option(&options, option.first, option.second);
	}

	const int result = avformat_open_input(&format_context, InputName().c_str(), input_format, &options);
	av_dict_free(&options);
	if (result < 0) {
		throw InvalidFile("Unable to open screen capture input: " + std::string(av_err2str(result)), InputName());
	}
}

void ScreenCaptureReader::OpenDecoder()
{
	if (avformat_find_stream_info(format_context, nullptr) < 0) {
		throw InvalidFile("Unable to read capture stream information.", InputName());
	}

	for (unsigned int index = 0; index < format_context->nb_streams; ++index) {
		if (format_context->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream = static_cast<int>(index);
			break;
		}
	}
	if (video_stream < 0) {
		throw InvalidFile("No video stream found in capture input.", InputName());
	}

	AVStream* stream = format_context->streams[video_stream];
	const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		throw InvalidCodec("No decoder available for capture stream.", InputName());
	}

	codec_context = ffmpeg_get_codec_context(stream, codec);
	if (!codec_context || avcodec_open2(codec_context, codec, nullptr) < 0) {
		throw InvalidCodec("Unable to open capture stream decoder.", InputName());
	}

	info.width = codec_context->width > 0 ? codec_context->width : settings.width;
	info.height = codec_context->height > 0 ? codec_context->height : settings.height;
	info.video_stream_index = video_stream;
	info.pixel_format = codec_context->pix_fmt;
	info.display_ratio = Fraction(info.width, info.height);
	info.display_ratio.Reduce();
	if (codec_context->framerate.num > 0 && codec_context->framerate.den > 0) {
		info.fps = Fraction(codec_context->framerate.num, codec_context->framerate.den);
		info.video_timebase = info.fps.Reciprocal();
	} else if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
		info.fps = Fraction(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
		info.video_timebase = info.fps.Reciprocal();
	} else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
		info.fps = Fraction(stream->r_frame_rate.num, stream->r_frame_rate.den);
		info.video_timebase = info.fps.Reciprocal();
	}

	source_frame = av_frame_alloc();
	rgba_frame = av_frame_alloc();
	packet = av_packet_alloc();
	if (!source_frame || !rgba_frame || !packet) {
		throw OutOfMemory("Unable to allocate capture decoder frames.", InputName());
	}
}

std::shared_ptr<Frame> ScreenCaptureReader::GetFrame(int64_t number)
{
	if (backend_reader) {
		if (!backend_reader->IsOpen()) {
			throw ReaderClosed("The ScreenCaptureReader is closed. Call Open() before GetFrame().");
		}
		const std::lock_guard<std::recursive_mutex> lock(getFrameMutex);
		return backend_reader->GetFrame(number);
	}
	if (!is_open) {
		throw ReaderClosed("The ScreenCaptureReader is closed. Call Open() before GetFrame().");
	}

	const std::lock_guard<std::recursive_mutex> lock(getFrameMutex);
	return DecodeNextFrame(number);
}

std::shared_ptr<Frame> ScreenCaptureReader::DecodeNextFrame(int64_t number)
{
	while (av_read_frame(format_context, packet) >= 0) {
		if (packet->stream_index != video_stream) {
			av_packet_unref(packet);
			dropped_packets++;
			continue;
		}

		const int send_result = avcodec_send_packet(codec_context, packet);
		av_packet_unref(packet);
		if (send_result < 0) {
			continue;
		}

		const int receive_result = avcodec_receive_frame(codec_context, source_frame);
		if (receive_result == AVERROR(EAGAIN)) {
			continue;
		}
		if (receive_result < 0) {
			throw InvalidFile("Unable to decode capture frame: " + std::string(av_err2str(receive_result)), InputName());
		}

		const int width = source_frame->width > 0 ? source_frame->width : info.width;
		const int height = source_frame->height > 0 ? source_frame->height : info.height;
		const PixelFormat src_fmt = static_cast<PixelFormat>(source_frame->format);

		sws_context = sws_getCachedContext(
			sws_context,
			width,
			height,
			src_fmt,
			width,
			height,
			AV_PIX_FMT_RGBA,
			SWS_BILINEAR,
			nullptr,
			nullptr,
			nullptr);
		if (!sws_context) {
			throw InvalidFile("Unable to create capture pixel conversion context.", InputName());
		}

		const int bytes_per_pixel = 4;
		unsigned char* buffer = static_cast<unsigned char*>(malloc(static_cast<size_t>(width) * height * bytes_per_pixel));
		if (!buffer) {
			throw OutOfMemory("Unable to allocate capture frame buffer.", InputName());
		}

		av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize, buffer, AV_PIX_FMT_RGBA, width, height, 1);
		const int scaled_lines = sws_scale(sws_context, source_frame->data, source_frame->linesize, 0, height, rgba_frame->data, rgba_frame->linesize);
		av_frame_unref(source_frame);

		if (scaled_lines <= 0) {
			free(buffer);
			continue;
		}

		auto frame = std::make_shared<Frame>(number, width, height, "#000000");
		frame->AddImage(width, height, bytes_per_pixel, QImage::Format_RGBA8888, buffer);
		frames_read++;
		return frame;
	}

	throw InvalidFile("Capture input ended before a frame could be read.", InputName());
}

void ScreenCaptureReader::Close()
{
	if (backend_reader) {
		backend_reader->Close();
	}
	if (packet) {
		av_packet_free(&packet);
	}
	if (source_frame) {
		av_frame_free(&source_frame);
	}
	if (rgba_frame) {
		av_frame_free(&rgba_frame);
	}
	if (sws_context) {
		sws_freeContext(sws_context);
		sws_context = nullptr;
	}
	if (codec_context) {
		avcodec_free_context(&codec_context);
	}
	if (format_context) {
		avformat_close_input(&format_context);
	}
	is_open = false;
	video_stream = -1;
}

openshot::CaptureReaderStats ScreenCaptureReader::GetStats() const
{
	if (backend_reader) {
		return backend_reader->GetStats();
	}
	CaptureReaderStats stats;
	stats.is_open = is_open;
	stats.frames_read = frames_read;
	stats.dropped_packets = dropped_packets;
	const double fps = settings.fps.den != 0 ? static_cast<double>(settings.fps.num) / static_cast<double>(settings.fps.den) : 0.0;
	stats.duration = fps > 0.0 ? static_cast<double>(frames_read) / fps : 0.0;
	return stats;
}

std::string ScreenCaptureReader::Json() const
{
	return JsonValue().toStyledString();
}

Json::Value ScreenCaptureReader::JsonValue() const
{
	Json::Value root = ReaderBase::JsonValue();
	root["type"] = "ScreenCaptureReader";
	root["backend"] = settings.backend;
	root["display"] = settings.display;
	root["x"] = settings.x;
	root["y"] = settings.y;
	root["width"] = settings.width;
	root["height"] = settings.height;
	root["fps"]["num"] = settings.fps.num;
	root["fps"]["den"] = settings.fps.den;
	root["include_cursor"] = settings.include_cursor;
	root["show_region"] = settings.show_region;
	root["options"] = Json::Value(Json::objectValue);
	for (const auto& option : settings.options) {
		root["options"][option.first] = option.second;
	}
	return root;
}

void ScreenCaptureReader::SetJson(const std::string value)
{
	try {
		SetJsonValue(openshot::stringToJson(value));
	} catch (const std::exception&) {
		throw InvalidJSON("JSON is invalid (missing keys or invalid data types)");
	}
}

void ScreenCaptureReader::SetJsonValue(const Json::Value root)
{
	if (!root["backend"].isNull())
		settings.backend = static_cast<ScreenCaptureBackend>(root["backend"].asInt());
	if (!root["display"].isNull())
		settings.display = root["display"].asString();
	if (!root["x"].isNull())
		settings.x = root["x"].asInt();
	if (!root["y"].isNull())
		settings.y = root["y"].asInt();
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
	if (!root["include_cursor"].isNull())
		settings.include_cursor = root["include_cursor"].asBool();
	if (!root["show_region"].isNull())
		settings.show_region = root["show_region"].asBool();
	if (!root["options"].isNull() && root["options"].isObject()) {
		settings.options.clear();
		for (const auto& key : root["options"].getMemberNames()) {
			settings.options[key] = root["options"][key].asString();
		}
	}
	if (settings.backend == SCREEN_CAPTURE_AUTO) {
		settings.backend = DefaultBackend();
	}
	ValidateSettings();
	PopulateInfo();
}
