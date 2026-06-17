/**
 * @file
 * @brief Wayland screen capture backend using xdg-desktop-portal and PipeWire
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ScreenCaptureReader.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/param/video/format-utils.h>
#include <unistd.h>

#include "Exceptions.h"
#include "Frame.h"
#include "ZmqLogger.h"

using namespace openshot;

namespace
{
	const char* PORTAL_BUS = "org.freedesktop.portal.Desktop";
	const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";
	const char* SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";

	std::string gerror_message(GError* error)
	{
		if (!error) {
			return "unknown error";
		}
		std::string message = error->message ? error->message : "unknown error";
		g_error_free(error);
		return message;
	}

	std::string token_for(const char* prefix)
	{
		static uint64_t counter = 0;
		std::stringstream token;
		token << prefix << "_" << ++counter;
		return token.str();
	}

	struct PortalResponse
	{
		bool done = false;
		bool timed_out = false;
		uint32_t code = 1;
		GVariant* results = nullptr;
		GMainLoop* loop = nullptr;

		~PortalResponse()
		{
			if (results) {
				g_variant_unref(results);
			}
		}
	};

	void portal_response_callback(
		GDBusConnection*,
		const gchar*,
		const gchar*,
		const gchar*,
		const gchar*,
		GVariant* parameters,
		gpointer user_data)
	{
		auto* response = static_cast<PortalResponse*>(user_data);
		GVariant* results = nullptr;
		g_variant_get(parameters, "(u@a{sv})", &response->code, &results);
		response->results = results;
		response->done = true;
		if (response->loop) {
			g_main_loop_quit(response->loop);
		}
	}

	gboolean portal_response_timeout(gpointer user_data)
	{
		auto* response = static_cast<PortalResponse*>(user_data);
		response->timed_out = true;
		if (response->loop) {
			g_main_loop_quit(response->loop);
		}
		return G_SOURCE_REMOVE;
	}

	GVariant* wait_for_portal_response(GDBusConnection* connection, const std::string& handle)
	{
		PortalResponse response;
		response.loop = g_main_loop_new(nullptr, FALSE);
		const guint timeout = g_timeout_add_seconds(60, portal_response_timeout, &response);
		const guint subscription = g_dbus_connection_signal_subscribe(
			connection,
			PORTAL_BUS,
			"org.freedesktop.portal.Request",
			"Response",
			handle.c_str(),
			nullptr,
			G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
			portal_response_callback,
			&response,
			nullptr);

		g_main_loop_run(response.loop);
		g_dbus_connection_signal_unsubscribe(connection, subscription);
		if (!response.timed_out) {
			g_source_remove(timeout);
		}
		g_main_loop_unref(response.loop);
		response.loop = nullptr;

		if (response.timed_out) {
			throw InvalidOptions("Timed out waiting for Wayland screen capture permission.");
		}
		if (!response.done || response.code != 0) {
			throw InvalidOptions("Wayland screen capture permission was denied or cancelled.");
		}
		GVariant* results = response.results;
		response.results = nullptr;
		return results;
	}

	std::string call_portal_request(
		GDBusConnection* connection,
		const char* method,
		GVariant* parameters)
	{
		GError* error = nullptr;
		GVariant* result = g_dbus_connection_call_sync(
			connection,
			PORTAL_BUS,
			PORTAL_PATH,
			SCREENCAST_IFACE,
			method,
			parameters,
			G_VARIANT_TYPE("(o)"),
			G_DBUS_CALL_FLAGS_NONE,
			-1,
			nullptr,
			&error);
		if (!result) {
			throw InvalidOptions("Wayland portal " + std::string(method) + " failed: " + gerror_message(error));
		}

		const char* handle = nullptr;
		g_variant_get(result, "(&o)", &handle);
		std::string handle_path = handle ? handle : "";
		g_variant_unref(result);
		return handle_path;
	}

	struct PortalStreamInfo
	{
		uint32_t node_id = 0;
		uint64_t pipewire_serial = 0;
		uint32_t source_type = 0;
		int width = 0;
		int height = 0;
	};

	PortalStreamInfo stream_info_from_results(GVariant* results)
	{
		GVariant* streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
		if (!streams || g_variant_n_children(streams) == 0) {
			if (streams) {
				g_variant_unref(streams);
			}
			throw InvalidOptions("Wayland portal did not return a PipeWire stream.");
		}

		PortalStreamInfo stream_info;
		GVariant* properties = nullptr;
		GVariant* child = g_variant_get_child_value(streams, 0);
		g_variant_get(child, "(u@a{sv})", &stream_info.node_id, &properties);
		if (properties) {
			uint64_t serial = 0;
			uint32_t source_type = 0;
			int width = 0;
			int height = 0;
			if (g_variant_lookup(properties, "pipewire-serial", "t", &serial)) {
				stream_info.pipewire_serial = serial;
			}
			if (g_variant_lookup(properties, "source_type", "u", &source_type)) {
				stream_info.source_type = source_type;
			}
			if (g_variant_lookup(properties, "size", "(ii)", &width, &height)) {
				stream_info.width = width;
				stream_info.height = height;
			}
			g_variant_unref(properties);
		}
		g_variant_unref(child);
		g_variant_unref(streams);
		return stream_info;
	}

	int open_pipewire_remote(GDBusConnection* connection, const std::string& session_handle)
	{
		GVariantBuilder options;
		g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

		GError* error = nullptr;
		GUnixFDList* out_fds = nullptr;
		GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
			connection,
			PORTAL_BUS,
			PORTAL_PATH,
			SCREENCAST_IFACE,
			"OpenPipeWireRemote",
			g_variant_new("(oa{sv})", session_handle.c_str(), &options),
			G_VARIANT_TYPE("(h)"),
			G_DBUS_CALL_FLAGS_NONE,
			-1,
			nullptr,
			&out_fds,
			nullptr,
			&error);
		if (!result) {
			throw InvalidOptions("Wayland portal OpenPipeWireRemote failed: " + gerror_message(error));
		}

		int fd_index = -1;
		g_variant_get(result, "(h)", &fd_index);
		g_variant_unref(result);
		int fd = g_unix_fd_list_get(out_fds, fd_index, &error);
		g_object_unref(out_fds);
		if (fd < 0) {
			throw InvalidOptions("Unable to obtain PipeWire remote file descriptor: " + gerror_message(error));
		}
		return fd;
	}

	struct CapturedFrame
	{
		int width = 0;
		int height = 0;
		std::vector<unsigned char> rgba;
	};
}

class WaylandScreenCaptureReader final : public ScreenCaptureReader::CaptureBackendReader
{
public:
	WaylandScreenCaptureReader(const ScreenCaptureSettings& new_settings, ReaderInfo& new_info)
		: settings(new_settings)
		, info(new_info)
		, connection(nullptr)
		, thread_loop(nullptr)
		, context(nullptr)
		, core(nullptr)
		, stream(nullptr)
		, session_closed_subscription(0)
		, frames_read(0)
		, dropped_packets(0)
		, open(false)
		, streaming(false)
		, stream_error(false)
		, crop_logged(false)
	{
	}

	~WaylandScreenCaptureReader() override
	{
		Close();
	}

	void Open() override
	{
		if (open) {
			return;
		}

		GError* error = nullptr;
		connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
		if (!connection) {
			throw InvalidOptions("Unable to connect to the session bus for Wayland capture: " + gerror_message(error));
		}

		session_handle = CreatePortalSession();
		SubscribePortalSessionClosed();
		SelectPortalSources(session_handle);
		stream_info = StartPortalSession(session_handle);
		ApplyPortalStreamInfo();
		ZmqLogger::Instance()->Log(
			"Wayland portal stream selected: node_id=" + std::to_string(stream_info.node_id) +
			" pipewire_serial=" + std::to_string(stream_info.pipewire_serial) +
			" source_type=" + std::to_string(stream_info.source_type) +
			" portal_size=" + std::to_string(stream_info.width) + "x" + std::to_string(stream_info.height));
		const int pipewire_fd = open_pipewire_remote(connection, session_handle);
		OpenPipeWireStream(pipewire_fd);
		open = true;
	}

	void Close() override
	{
		open = false;
		streaming = false;

		if (thread_loop) {
			pw_thread_loop_stop(thread_loop);
		}
		if (connection && session_closed_subscription) {
			g_dbus_connection_signal_unsubscribe(connection, session_closed_subscription);
			session_closed_subscription = 0;
		}
		if (connection && !session_handle.empty()) {
			GError* error = nullptr;
			GVariant* result = g_dbus_connection_call_sync(
				connection,
				PORTAL_BUS,
				session_handle.c_str(),
				"org.freedesktop.portal.Session",
				"Close",
				nullptr,
				nullptr,
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				nullptr,
				&error);
			if (result) {
				g_variant_unref(result);
			} else if (error) {
				g_error_free(error);
			}
			session_handle.clear();
		}
		if (stream) {
			pw_stream_destroy(stream);
			stream = nullptr;
		}
		if (core) {
			pw_core_disconnect(core);
			core = nullptr;
		}
		if (context) {
			pw_context_destroy(context);
			context = nullptr;
		}
		if (thread_loop) {
			pw_thread_loop_destroy(thread_loop);
			thread_loop = nullptr;
		}
		if (connection) {
			g_object_unref(connection);
			connection = nullptr;
		}
	}

	bool IsOpen() const override
	{
		return open;
	}

	CaptureReaderStats GetStats() const override
	{
		CaptureReaderStats stats;
		stats.is_open = open;
		stats.frames_read = frames_read;
		stats.dropped_packets = dropped_packets;
		const double fps = settings.fps.den != 0 ? static_cast<double>(settings.fps.num) / static_cast<double>(settings.fps.den) : 0.0;
		stats.duration = fps > 0.0 ? static_cast<double>(frames_read) / fps : 0.0;
		return stats;
	}

	std::shared_ptr<Frame> GetFrame(int64_t number) override
	{
		CapturedFrame captured;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (frame_queue.empty() && !stream_error && open && std::chrono::steady_clock::now() < deadline) {
				lock.unlock();
				while (g_main_context_iteration(nullptr, FALSE)) {
				}
				lock.lock();
				queue_condition.wait_for(lock, std::chrono::milliseconds(100));
			}
			if (frame_queue.empty() && !stream_error && open) {
				throw InvalidFile("Timed out waiting for a Wayland capture frame.", "wayland");
			}
			if (stream_error) {
				throw InvalidFile("Wayland capture stream failed.", "wayland");
			}
			if (frame_queue.empty()) {
				throw ReaderClosed("The Wayland screen capture stream is closed.");
			}
			captured = std::move(frame_queue.front());
			frame_queue.pop_front();
		}

		const int bytes_per_pixel = 4;
		const size_t buffer_size = static_cast<size_t>(captured.width) * captured.height * bytes_per_pixel;
		unsigned char* buffer = static_cast<unsigned char*>(malloc(buffer_size));
		if (!buffer) {
			throw OutOfMemory("Unable to allocate Wayland capture frame buffer.", "wayland");
		}
		std::memcpy(buffer, captured.rgba.data(), buffer_size);

		auto frame = std::make_shared<Frame>(number, captured.width, captured.height, "#000000");
		frame->AddImage(captured.width, captured.height, bytes_per_pixel, QImage::Format_RGBA8888, buffer);
		frames_read++;
		return frame;
	}

private:
	std::string CreatePortalSession()
	{
		GVariantBuilder options;
		g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token_for("openshot_create").c_str()));
		g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(token_for("openshot_session").c_str()));

		const std::string handle = call_portal_request(connection, "CreateSession", g_variant_new("(a{sv})", &options));
		GVariant* results = wait_for_portal_response(connection, handle);

		const char* session = nullptr;
		if (!g_variant_lookup(results, "session_handle", "&s", &session) || !session) {
			g_variant_unref(results);
			throw InvalidOptions("Wayland portal did not return a screencast session handle.");
		}
		std::string session_handle = session;
		g_variant_unref(results);
		return session_handle;
	}

	void SubscribePortalSessionClosed()
	{
		if (!connection || session_handle.empty()) {
			return;
		}
		session_closed_subscription = g_dbus_connection_signal_subscribe(
			connection,
			PORTAL_BUS,
			"org.freedesktop.portal.Session",
			"Closed",
			session_handle.c_str(),
			nullptr,
			G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
			OnPortalSessionClosed,
			this,
			nullptr);
	}

	void SelectPortalSources(const std::string& session_handle)
	{
		const uint32_t source_monitor = 1;
		const uint32_t source_window = 2;
		const uint32_t cursor_hidden = 1;
		const uint32_t cursor_embedded = 2;

		GVariantBuilder options;
		g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token_for("openshot_select").c_str()));
		g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(source_monitor | source_window));
		g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
		g_variant_builder_add(&options, "{sv}", "cursor_mode", g_variant_new_uint32(settings.include_cursor ? cursor_embedded : cursor_hidden));

		const std::string handle = call_portal_request(
			connection,
			"SelectSources",
			g_variant_new("(oa{sv})", session_handle.c_str(), &options));
		GVariant* results = wait_for_portal_response(connection, handle);
		g_variant_unref(results);
	}

	PortalStreamInfo StartPortalSession(const std::string& session_handle)
	{
		GVariantBuilder options;
		g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token_for("openshot_start").c_str()));

		const std::string handle = call_portal_request(
			connection,
			"Start",
			g_variant_new("(osa{sv})", session_handle.c_str(), "", &options));
		GVariant* results = wait_for_portal_response(connection, handle);
		const PortalStreamInfo result_stream_info = stream_info_from_results(results);
		g_variant_unref(results);
		return result_stream_info;
	}

	void ApplyPortalStreamInfo()
	{
		if (stream_info.width <= 0 || stream_info.height <= 0) {
			return;
		}
		info.width = stream_info.width;
		info.height = stream_info.height;
		info.display_ratio = Fraction(stream_info.width, stream_info.height);
		info.display_ratio.Reduce();
	}

	void OpenPipeWireStream(int pipewire_fd)
	{
		pw_init(nullptr, nullptr);

		thread_loop = pw_thread_loop_new("openshot-wayland-capture", nullptr);
		if (!thread_loop) {
			throw InvalidOptions("Unable to create PipeWire thread loop.");
		}
		context = pw_context_new(pw_thread_loop_get_loop(thread_loop), nullptr, 0);
		if (!context) {
			throw InvalidOptions("Unable to create PipeWire context.");
		}
		core = pw_context_connect_fd(context, pipewire_fd, nullptr, 0);
		if (!core) {
			close(pipewire_fd);
			throw InvalidOptions("Unable to connect to the portal PipeWire remote.");
		}

		pw_properties* props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Screen",
			nullptr);
		if (stream_info.pipewire_serial > 0) {
			pw_properties_set(props, PW_KEY_TARGET_OBJECT, std::to_string(stream_info.pipewire_serial).c_str());
		}
		stream = pw_stream_new(core, "OpenShot Wayland Screen Capture", props);
		if (!stream) {
			throw InvalidOptions("Unable to create PipeWire capture stream.");
		}

		pw_stream_add_listener(stream, &stream_listener, &stream_events, this);

		if (pw_thread_loop_start(thread_loop) < 0) {
			throw InvalidOptions("Unable to start PipeWire capture thread loop.");
		}

		uint8_t format_buffer[1024];
		spa_pod_builder builder = SPA_POD_BUILDER_INIT(format_buffer, sizeof(format_buffer));
		const spa_pod* params[1];
		params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
			&builder,
			SPA_TYPE_OBJECT_Format,
			SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,
			SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,
			SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,
			SPA_POD_CHOICE_ENUM_Id(
				4,
				SPA_VIDEO_FORMAT_BGRx,
				SPA_VIDEO_FORMAT_RGBA,
				SPA_VIDEO_FORMAT_BGRA,
				SPA_VIDEO_FORMAT_RGBx)));

		pw_thread_loop_lock(thread_loop);
		const uint32_t target_id = stream_info.pipewire_serial > 0 ? PW_ID_ANY : stream_info.node_id;
		const int result = pw_stream_connect(
			stream,
			PW_DIRECTION_INPUT,
			target_id,
			static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
			params,
			1);
		if (result < 0) {
			pw_thread_loop_unlock(thread_loop);
			throw InvalidOptions("Unable to connect to PipeWire capture stream.");
		}

		while (!streaming && !stream_error) {
			if (pw_thread_loop_timed_wait(thread_loop, 10) < 0) {
				stream_error = true;
				break;
			}
		}
		pw_thread_loop_unlock(thread_loop);

		if (stream_error) {
			throw InvalidOptions("PipeWire capture stream failed to start or timed out.");
		}
	}

	static void OnPortalSessionClosed(
		GDBusConnection*,
		const gchar*,
		const gchar*,
		const gchar*,
		const gchar*,
		GVariant*,
		gpointer user_data)
	{
		auto* self = static_cast<WaylandScreenCaptureReader*>(user_data);
		self->open = false;
		self->stream_error = true;
		if (self->thread_loop) {
			pw_thread_loop_signal(self->thread_loop, false);
		}
		self->queue_condition.notify_all();
	}

	static void OnStreamStateChanged(void* data, pw_stream_state, pw_stream_state state, const char*)
	{
		auto* self = static_cast<WaylandScreenCaptureReader*>(data);
		if (state == PW_STREAM_STATE_STREAMING) {
			self->streaming = true;
			pw_thread_loop_signal(self->thread_loop, false);
		} else if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
			self->stream_error = true;
			pw_thread_loop_signal(self->thread_loop, false);
			self->queue_condition.notify_all();
		}
	}

	static void OnStreamParamChanged(void* data, uint32_t id, const spa_pod* param)
	{
		if (id != SPA_PARAM_Format || !param) {
			return;
		}
		auto* self = static_cast<WaylandScreenCaptureReader*>(data);
		spa_video_info info = {};
		if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0 ||
				info.media_type != SPA_MEDIA_TYPE_video ||
				info.media_subtype != SPA_MEDIA_SUBTYPE_raw ||
				spa_format_video_raw_parse(param, &info.info.raw) < 0) {
			return;
		}

		self->video_format = info.info.raw.format;
		self->stream_width = static_cast<int>(info.info.raw.size.width);
		self->stream_height = static_cast<int>(info.info.raw.size.height);
		if (self->stream_width > 0 && self->stream_height > 0) {
			ZmqLogger::Instance()->Log(
				"Wayland PipeWire stream format: " +
				std::to_string(self->stream_width) + "x" + std::to_string(self->stream_height) +
				" format=" + std::to_string(self->video_format));
			self->info.width = self->stream_width;
			self->info.height = self->stream_height;
			self->info.display_ratio = Fraction(self->stream_width, self->stream_height);
			self->info.display_ratio.Reduce();
		}
	}

	static void OnStreamProcess(void* data)
	{
		auto* self = static_cast<WaylandScreenCaptureReader*>(data);
		pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
		if (!buffer) {
			return;
		}

		self->CopyPipeWireBuffer(buffer);
		pw_stream_queue_buffer(self->stream, buffer);
	}

	void CopyPipeWireBuffer(pw_buffer* buffer)
	{
		spa_buffer* spa_buffer = buffer->buffer;
		if (!spa_buffer || spa_buffer->n_datas == 0 || !spa_buffer->datas[0].data || stream_width <= 0 || stream_height <= 0) {
			dropped_packets++;
			return;
		}

		spa_data& data = spa_buffer->datas[0];
		const spa_chunk* chunk = data.chunk;
		const uint8_t* src = static_cast<const uint8_t*>(data.data) + (chunk ? chunk->offset : 0);
		const int stride = chunk && chunk->stride > 0 ? chunk->stride : stream_width * 4;
		const int readable_rows = chunk && chunk->size > 0 ? static_cast<int>(chunk->size) / stride : stream_height;
		int crop_x = 0;
		int crop_y = 0;
		int crop_width = stream_width;
		int crop_height = stream_height;
		auto* crop = static_cast<spa_meta_region*>(
			spa_buffer_find_meta_data(spa_buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
		if (crop && spa_meta_region_is_valid(crop)) {
			crop_x = std::max(0, crop->region.position.x);
			crop_y = std::max(0, crop->region.position.y);
			crop_width = std::min(static_cast<int>(crop->region.size.width), stream_width - crop_x);
			crop_height = std::min(static_cast<int>(crop->region.size.height), stream_height - crop_y);
			if (!crop_logged) {
				ZmqLogger::Instance()->Log(
					"Wayland PipeWire video crop: x=" + std::to_string(crop_x) +
					" y=" + std::to_string(crop_y) +
					" width=" + std::to_string(crop_width) +
					" height=" + std::to_string(crop_height));
				crop_logged = true;
			}
		}
		if (crop_width <= 0 || crop_height <= 0) {
			dropped_packets++;
			return;
		}
		if (crop_width % 2 != 0) {
			crop_width--;
		}
		if (crop_height % 2 != 0) {
			crop_height--;
		}
		if (crop_width <= 0 || crop_height <= 0) {
			dropped_packets++;
			return;
		}
		const int rows = std::min(crop_height, readable_rows - crop_y);
		if (rows <= 0) {
			dropped_packets++;
			return;
		}

		CapturedFrame frame;
		frame.width = crop_width;
		frame.height = crop_height;
		frame.rgba.assign(static_cast<size_t>(frame.width) * frame.height * 4, 0);

		for (int y = 0; y < rows; ++y) {
			const uint8_t* row = src + static_cast<size_t>(y + crop_y) * stride;
			for (int x = 0; x < crop_width; ++x) {
				const uint8_t* pixel = row + static_cast<size_t>(x + crop_x) * 4;
				uint8_t r = 0;
				uint8_t g = 0;
				uint8_t b = 0;
				uint8_t a = 255;
				switch (video_format) {
				case SPA_VIDEO_FORMAT_RGBA:
					r = pixel[0]; g = pixel[1]; b = pixel[2]; a = pixel[3];
					break;
				case SPA_VIDEO_FORMAT_RGBx:
					r = pixel[0]; g = pixel[1]; b = pixel[2];
					break;
				case SPA_VIDEO_FORMAT_BGRA:
					b = pixel[0]; g = pixel[1]; r = pixel[2]; a = pixel[3];
					break;
				case SPA_VIDEO_FORMAT_BGRx:
				default:
					b = pixel[0]; g = pixel[1]; r = pixel[2];
					break;
				}
				const size_t dst = (static_cast<size_t>(y) * crop_width + x) * 4;
				frame.rgba[dst + 0] = r;
				frame.rgba[dst + 1] = g;
				frame.rgba[dst + 2] = b;
				frame.rgba[dst + 3] = a;
			}
		}

		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			if (frame_queue.size() > 3) {
				frame_queue.pop_front();
				dropped_packets++;
			}
			frame_queue.push_back(std::move(frame));
		}
		if (info.width != crop_width || info.height != crop_height) {
			info.width = crop_width;
			info.height = crop_height;
			info.display_ratio = Fraction(crop_width, crop_height);
			info.display_ratio.Reduce();
		}
		queue_condition.notify_one();
	}

	ScreenCaptureSettings settings;
	ReaderInfo& info;
	GDBusConnection* connection;
	pw_thread_loop* thread_loop;
	pw_context* context;
	pw_core* core;
	pw_stream* stream;
	spa_hook stream_listener;
	PortalStreamInfo stream_info;
	std::string session_handle;
	guint session_closed_subscription;
	int stream_width = 0;
	int stream_height = 0;
	spa_video_format video_format = SPA_VIDEO_FORMAT_BGRx;
	int64_t frames_read;
	int dropped_packets;
	bool open;
	bool streaming;
	bool stream_error;
	bool crop_logged;
	std::mutex queue_mutex;
	std::condition_variable queue_condition;
	std::deque<CapturedFrame> frame_queue;

	static const pw_stream_events stream_events;
};

const pw_stream_events WaylandScreenCaptureReader::stream_events = {
	PW_VERSION_STREAM_EVENTS,
	nullptr,
	WaylandScreenCaptureReader::OnStreamStateChanged,
	nullptr,
	nullptr,
	WaylandScreenCaptureReader::OnStreamParamChanged,
	nullptr,
	nullptr,
	WaylandScreenCaptureReader::OnStreamProcess,
	nullptr,
	nullptr,
	nullptr
};

std::unique_ptr<ScreenCaptureReader::CaptureBackendReader> CreateWaylandScreenCaptureReader(
	const ScreenCaptureSettings& settings,
	ReaderInfo& info)
{
	return std::make_unique<WaylandScreenCaptureReader>(settings, info);
}
