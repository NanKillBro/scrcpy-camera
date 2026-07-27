/*
 * scrcpy source scaffold
 * Copyright (C) 2026 NanKill <nankill@nankill.xyz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>
 */

#include <obs-module.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "scrcpy-session.h"
#include "scrcpy-source.h"

#ifdef _WIN32
#include <stdio.h>
#include <windows.h>
#endif

#define ADB_CMD_TIMEOUT_MS 3000

#define SETTING_ADB_PATH "adb_path"
#define SETTING_DEVICE_SERIAL "device_serial"
#define SETTING_SERVER_JAR_PATH "server_jar_path"
#define SETTING_SCRCPY_VERSION "scrcpy_version"
#define SETTING_LOCAL_PORT "local_port"
#define SETTING_VIDEO_CODEC "video_codec"
#define SETTING_VIDEO_BIT_RATE "video_bit_rate"
#define SETTING_MAX_SIZE "max_size"
#define SETTING_VIDEO_SOURCE "video_source"
#define SETTING_CAMERA_ID "camera_id"
#define SETTING_CAMERA_SIZE "camera_size"
#define SETTING_HW_DECODING "hw_decoding"
#define SETTING_AUDIO_ENABLED "audio_enabled"
#define SETTING_AUDIO_SOURCE "audio_source"
#define SETTING_AUDIO_CODEC "audio_codec"
#define SETTING_AUDIO_BIT_RATE "audio_bit_rate"
#define SETTING_LOW_LATENCY "low_latency"

#ifdef _WIN32
static const char *const DEFAULT_ADB_PATH = "adb.exe";
#define ADB_FILTER "Executable (*.exe);;All Files (*.*)"
#else
static const char *const DEFAULT_ADB_PATH = "adb";
#define ADB_FILTER "All Files (*.*)"
#endif
static const char *const DEFAULT_SCRCPY_VERSION = "4.0";

struct scrcpy_source {
	obs_source_t *source;
	struct scrcpy_session *session;
	char *adb_path;
	char *device_serial;
	char *server_jar_path;
	char *scrcpy_version;
	char *video_codec;
	char *video_source;
	char *camera_id;
	char *camera_size;
	char *audio_source;
	char *audio_codec;
	uint16_t local_port;
	uint32_t video_bit_rate;
	uint32_t audio_bit_rate;
	uint16_t max_size;
	uint32_t frame_width;
	uint32_t frame_height;
	bool hw_decoding;
	bool audio_enabled;
	/* Low-latency level: 0=Off, 1=Low, 2=Medium, 3=High */
	uint8_t low_latency_level;
	bool active;
	bool restart_pending;
	uint64_t restart_after_ns;
};

static void scrcpy_source_update(void *data, obs_data_t *settings);
static int scrcpy_refresh_device_list(struct scrcpy_source *context, obs_property_t *list);
static bool scrcpy_refresh_button_clicked(obs_properties_t *props, obs_property_t *button, void *data);
static void scrcpy_source_start_session(struct scrcpy_source *context);
static void scrcpy_source_tick(void *data, float seconds);
static void scrcpy_source_stop_session(struct scrcpy_source *context);
static void scrcpy_source_on_frame(void *opaque, const struct obs_source_frame *frame);
static void scrcpy_source_on_audio(void *opaque, const struct obs_source_audio *audio);

static uint32_t scrcpy_source_get_width(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return 0;
	return context->frame_width;
}

static uint32_t scrcpy_source_get_height(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return 0;
	return context->frame_height;
}

static const char *scrcpy_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "scrcpy Camera";
}

static void *scrcpy_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct scrcpy_source *context = bzalloc(sizeof(*context));
	context->source = source;
	context->session = scrcpy_session_create();
	context->frame_width = 0;
	context->frame_height = 0;

	obs_log(LOG_INFO, "creating scrcpy source scaffold");
	scrcpy_source_update(context, settings);
	return context;
}

static void scrcpy_source_destroy(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	bfree(context->adb_path);
	bfree(context->device_serial);
	bfree(context->server_jar_path);
	bfree(context->scrcpy_version);
	bfree(context->video_codec);
	bfree(context->video_source);
	bfree(context->camera_id);
	bfree(context->camera_size);
	bfree(context->audio_source);
	bfree(context->audio_codec);
	scrcpy_session_destroy(context->session);
	bfree(context);
}

static void scrcpy_source_update(void *data, obs_data_t *settings)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	const char *adb_path = obs_data_get_string(settings, SETTING_ADB_PATH);
	const char *device_serial = obs_data_get_string(settings, SETTING_DEVICE_SERIAL);
	const char *server_jar_path = obs_data_get_string(settings, SETTING_SERVER_JAR_PATH);
	const char *scrcpy_version = obs_data_get_string(settings, SETTING_SCRCPY_VERSION);
	const char *video_codec = obs_data_get_string(settings, SETTING_VIDEO_CODEC);
	const char *video_source = obs_data_get_string(settings, SETTING_VIDEO_SOURCE);
	const char *camera_id = obs_data_get_string(settings, SETTING_CAMERA_ID);
	const char *camera_size = obs_data_get_string(settings, SETTING_CAMERA_SIZE);
	const char *audio_source = obs_data_get_string(settings, SETTING_AUDIO_SOURCE);
	const char *audio_codec = obs_data_get_string(settings, SETTING_AUDIO_CODEC);
	long long local_port = obs_data_get_int(settings, SETTING_LOCAL_PORT);
	long long video_bit_rate = obs_data_get_int(settings, SETTING_VIDEO_BIT_RATE);
	long long audio_bit_rate = obs_data_get_int(settings, SETTING_AUDIO_BIT_RATE);
	long long max_size = obs_data_get_int(settings, SETTING_MAX_SIZE);
	bool hw_decoding = obs_data_get_bool(settings, SETTING_HW_DECODING);
	bool audio_enabled = obs_data_get_bool(settings, SETTING_AUDIO_ENABLED);

	bfree(context->adb_path);
	bfree(context->device_serial);
	bfree(context->server_jar_path);
	bfree(context->scrcpy_version);
	bfree(context->video_codec);
	bfree(context->video_source);
	bfree(context->camera_id);
	bfree(context->camera_size);
	bfree(context->audio_codec);
	context->adb_path = bstrdup(adb_path && adb_path[0] ? adb_path : DEFAULT_ADB_PATH);
	context->device_serial = bstrdup(device_serial ? device_serial : "");
	context->server_jar_path =
		bstrdup(server_jar_path && server_jar_path[0] ? server_jar_path : "scrcpy-server.jar");
	context->scrcpy_version =
		bstrdup(scrcpy_version && scrcpy_version[0] ? scrcpy_version : DEFAULT_SCRCPY_VERSION);
	context->video_codec = bstrdup(video_codec && video_codec[0] ? video_codec : "h264");
	context->video_source = bstrdup(video_source && video_source[0] ? video_source : "display");
	context->camera_id = bstrdup(camera_id && camera_id[0] ? camera_id : "0");
	context->camera_size = bstrdup(camera_size && camera_size[0] ? camera_size : "1920x1080");
	context->audio_source = bstrdup(audio_source && audio_source[0] ? audio_source : "output");
	context->audio_codec = bstrdup(audio_codec && audio_codec[0] ? audio_codec : "opus");

	/* OBS editable combo box uses the display text. Extract just the ID. */
	char *space = strchr(context->camera_id, ' ');
	if (space)
		*space = '\0';

	if (local_port < 1 || local_port > 65535)
		local_port = 27183;
	context->local_port = (uint16_t)local_port;
	if (video_bit_rate < 1)
		video_bit_rate = 8;
	context->video_bit_rate = (uint32_t)(video_bit_rate * 1000000);
	if (audio_bit_rate < 1)
		audio_bit_rate = 128;
	context->audio_bit_rate = (uint32_t)(audio_bit_rate * 1000);
	context->max_size = (uint16_t)max_size;
	context->hw_decoding = hw_decoding;
	context->audio_enabled = audio_enabled;

	/* Low-latency level: clamp to [0..3] range */
	long long low_latency = obs_data_get_int(settings, SETTING_LOW_LATENCY);
	if (low_latency < 0) low_latency = 0;
	if (low_latency > 3) low_latency = 3;
	context->low_latency_level = (uint8_t)low_latency;

	obs_log(LOG_INFO,
		"scrcpy source updated: device='%s', source=%s, codec=%s, bitrate=%uMbps, max_size=%hu, camera_size=%s, audio=%s(%s)",
		context->device_serial, context->video_source, context->video_codec, (uint32_t)video_bit_rate,
		context->max_size, context->camera_size, context->audio_enabled ? "on" : "off", context->audio_codec);

	if (context->active) {
		context->restart_pending = true;
		context->restart_after_ns = os_gettime_ns() + 800000000ULL; /* 800 ms debounce */
	}
}

static void scrcpy_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_ADB_PATH, DEFAULT_ADB_PATH);
	obs_data_set_default_string(settings, SETTING_DEVICE_SERIAL, "");
	obs_data_set_default_string(settings, SETTING_SERVER_JAR_PATH, "scrcpy-server.jar");
	obs_data_set_default_string(settings, SETTING_SCRCPY_VERSION, DEFAULT_SCRCPY_VERSION);
	obs_data_set_default_string(settings, SETTING_VIDEO_CODEC, "h264");
	obs_data_set_default_int(settings, SETTING_LOCAL_PORT, 27183);
	obs_data_set_default_int(settings, SETTING_VIDEO_BIT_RATE, 8);
	obs_data_set_default_int(settings, SETTING_MAX_SIZE, 0);
	obs_data_set_default_string(settings, SETTING_VIDEO_SOURCE, "display");
	obs_data_set_default_string(settings, SETTING_CAMERA_ID, "0");
	obs_data_set_default_string(settings, SETTING_CAMERA_SIZE, "1920x1080");
	obs_data_set_default_bool(settings, SETTING_HW_DECODING, true);
	obs_data_set_default_bool(settings, SETTING_AUDIO_ENABLED, false);
	obs_data_set_default_string(settings, SETTING_AUDIO_SOURCE, "output");
	obs_data_set_default_string(settings, SETTING_AUDIO_CODEC, "opus");
	obs_data_set_default_int(settings, SETTING_AUDIO_BIT_RATE, 128);
	obs_data_set_default_int(settings, SETTING_LOW_LATENCY, 0);
}

static bool scrcpy_video_source_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const char *val = obs_data_get_string(settings, SETTING_VIDEO_SOURCE);
	bool is_camera = (val && strcmp(val, "camera") == 0);
	obs_property_t *cam_id_prop = obs_properties_get(props, SETTING_CAMERA_ID);
	obs_property_set_visible(cam_id_prop, is_camera);
	obs_property_t *cam_size_prop = obs_properties_get(props, SETTING_CAMERA_SIZE);
	obs_property_set_visible(cam_size_prop, is_camera);
	return true;
}

static bool scrcpy_audio_enabled_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, SETTING_AUDIO_ENABLED);
	obs_property_t *source_prop = obs_properties_get(props, SETTING_AUDIO_SOURCE);
	obs_property_set_visible(source_prop, enabled);
	obs_property_t *codec_prop = obs_properties_get(props, SETTING_AUDIO_CODEC);
	obs_property_set_visible(codec_prop, enabled);
	obs_property_t *bitrate_prop = obs_properties_get(props, SETTING_AUDIO_BIT_RATE);
	obs_property_set_visible(bitrate_prop, enabled);
	return true;
}

static obs_properties_t *scrcpy_source_properties(void *unused)
{
	struct scrcpy_source *context = unused;
	obs_property_t *codec_list;
	obs_property_t *max_size_list;
	obs_property_t *vsource_list;
	obs_property_t *cam_id_prop;

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_path(props, SETTING_ADB_PATH, "ADB executable", OBS_PATH_FILE, ADB_FILTER, NULL);

	obs_property_t *device_list = obs_properties_add_list(props, SETTING_DEVICE_SERIAL, "ADB device",
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (device_list) {
		/*
		 * Auto-scanning adb here would block the OBS UI thread for several
		 * seconds while adb enumerates devices (and runs mdns services on
		 * refresh). Defer scanning to the explicit "Refresh device list"
		 * button so the properties panel opens instantly. Keep the currently
		 * configured device, if any, so the saved selection is preserved.
		 */
		obs_property_list_add_string(device_list, "(Click Refresh to scan)", "");
		if (context && context->device_serial && context->device_serial[0])
			obs_property_list_add_string(device_list, context->device_serial, context->device_serial);
	}

	obs_properties_add_button2(props, "refresh_devices", "Refresh device list", scrcpy_refresh_button_clicked, context);
	obs_properties_add_path(props, SETTING_SERVER_JAR_PATH, "scrcpy-server.jar path", OBS_PATH_FILE,
				"Jar Files (*.jar);;All Files (*.*)", NULL);
	obs_properties_add_text(props, SETTING_SCRCPY_VERSION, "scrcpy protocol version", OBS_TEXT_DEFAULT);

	vsource_list = obs_properties_add_list(props, SETTING_VIDEO_SOURCE, "Video source", OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(vsource_list, "Screen", "display");
	obs_property_list_add_string(vsource_list, "Camera", "camera");
	obs_property_set_modified_callback(vsource_list, scrcpy_video_source_changed);

	cam_id_prop = obs_properties_add_list(props, SETTING_CAMERA_ID, "Camera ID", OBS_COMBO_TYPE_EDITABLE,
					      OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(cam_id_prop, "0 (Back)", "0");
	obs_property_list_add_string(cam_id_prop, "1 (Front)", "1");
	obs_property_list_add_string(cam_id_prop, "2", "2");
	obs_property_list_add_string(cam_id_prop, "3", "3");

	obs_properties_add_text(props, SETTING_CAMERA_SIZE, "Camera Size", OBS_TEXT_DEFAULT);

	codec_list = obs_properties_add_list(props, SETTING_VIDEO_CODEC, "Video codec", OBS_COMBO_TYPE_LIST,
					     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec_list, "H.264 (AVC)", "h264");
	obs_property_list_add_string(codec_list, "H.265 (HEVC)", "h265");

	obs_properties_add_bool(props, SETTING_HW_DECODING, "Use Hardware Decoding");

	/* Low-latency dropdown — placed after HW Decoding since it's
	 * directly related to video pipeline performance tuning. */
	obs_property_t *latency_list = obs_properties_add_list(props, SETTING_LOW_LATENCY, "Low Latency",
								       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(latency_list, "Off (Default)", 0);
	obs_property_list_add_int(latency_list, "Low", 1);
	obs_property_list_add_int(latency_list, "Medium", 2);
	obs_property_list_add_int(latency_list, "High (Aggressive)", 3);

	obs_properties_add_int_slider(props, SETTING_VIDEO_BIT_RATE, "Video bitrate (Mbps)", 1, 50, 1);

	max_size_list = obs_properties_add_list(props, SETTING_MAX_SIZE, "Max resolution", OBS_COMBO_TYPE_LIST,
						OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(max_size_list, "Original (no limit)", 0);
	obs_property_list_add_int(max_size_list, "2560p (WQHD)", 2560);
	obs_property_list_add_int(max_size_list, "1920p (Full HD)", 1920);
	obs_property_list_add_int(max_size_list, "1280p (HD)", 1280);
	obs_property_list_add_int(max_size_list, "960p", 960);
	obs_property_list_add_int(max_size_list, "720p", 720);
	obs_property_list_add_int(max_size_list, "480p", 480);

	obs_properties_add_int(props, SETTING_LOCAL_PORT, "Local TCP port", 1, 65535, 1);

	/* Audio settings */
	obs_property_t *audio_enable =
		obs_properties_add_bool(props, SETTING_AUDIO_ENABLED, "Enable Audio (Android 11+)");
	obs_property_set_modified_callback(audio_enable, scrcpy_audio_enabled_changed);

	obs_property_t *audio_source_list = obs_properties_add_list(props, SETTING_AUDIO_SOURCE, "Audio Source",
								    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_source_list, "Internal Audio (Output)", "output");
	obs_property_list_add_string(audio_source_list, "Playback (Android 13+ Duplication)", "playback");
	obs_property_list_add_string(audio_source_list, "Microphone (Mic)", "mic");

	obs_property_t *audio_codec_list = obs_properties_add_list(props, SETTING_AUDIO_CODEC, "Audio codec",
								   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_codec_list, "Opus", "opus");
	obs_property_list_add_string(audio_codec_list, "AAC", "aac");
	obs_property_list_add_string(audio_codec_list, "FLAC", "flac");
	obs_property_list_add_string(audio_codec_list, "Raw (PCM)", "raw");

	obs_properties_add_int_slider(props, SETTING_AUDIO_BIT_RATE, "Audio bitrate (kbps)", 32, 320, 8);

	return props;
}

static void scrcpy_source_start_session(struct scrcpy_source *context)
{
	struct scrcpy_session_config config;

	if (!context || !context->session)
		return;

	config.adb_path = context->adb_path;
	config.device_serial = context->device_serial;
	config.server_jar_path = context->server_jar_path;
	config.scrcpy_version = context->scrcpy_version;
	config.video_codec = context->video_codec;
	config.video_source = context->video_source;
	config.camera_id = context->camera_id;
	config.camera_size = context->camera_size;
	config.local_port = context->local_port;
	config.video_bit_rate = context->video_bit_rate;
	config.max_size = context->max_size;
	config.hw_decoding = context->hw_decoding;
	config.audio_enabled = context->audio_enabled;
	config.audio_source = context->audio_source;
	config.audio_codec = context->audio_codec;
	config.audio_bit_rate = context->audio_bit_rate;
	config.low_latency_level = context->low_latency_level;
	config.on_frame = scrcpy_source_on_frame;
	config.on_frame_opaque = context;
	config.on_audio = scrcpy_source_on_audio;
	config.on_audio_opaque = context;

	if (scrcpy_session_start(context->session, &config) != 0)
		obs_log(LOG_WARNING, "unable to start scrcpy bootstrap session");
}

static void scrcpy_source_stop_session(struct scrcpy_source *context)
{
	if (!context || !context->session)
		return;

	scrcpy_session_stop(context->session);
}

static void scrcpy_source_on_frame(void *opaque, const struct obs_source_frame *frame)
{
	struct scrcpy_source *context = opaque;
	if (!context || !frame)
		return;

	context->frame_width = frame->width;
	context->frame_height = frame->height;
	obs_source_output_video(context->source, frame);
}

static void scrcpy_source_on_audio(void *opaque, const struct obs_source_audio *audio)
{
	struct scrcpy_source *context = opaque;
	if (!context || !audio)
		return;

	obs_source_output_audio(context->source, audio);
}

static void scrcpy_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct scrcpy_source *context = data;
	if (!context || !context->restart_pending)
		return;

	if (os_gettime_ns() < context->restart_after_ns)
		return;

	context->restart_pending = false;
	scrcpy_source_stop_session(context);
	scrcpy_source_start_session(context);
}

static void scrcpy_source_activate(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	context->active = true;
	scrcpy_source_start_session(context);
}

static void scrcpy_source_deactivate(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	context->restart_pending = false;
	context->active = false;
	scrcpy_source_stop_session(context);
}

static int scrcpy_parse_adb_devices_from_string(const char *text, obs_property_t *list)
{
	char buffer[4096];
	char *line;
	char *saveptr_outer = NULL;
	int found = 0;

	_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%s", text);

	for (line = strtok_s(buffer, "\r\n", &saveptr_outer); line; line = strtok_s(NULL, "\r\n", &saveptr_outer)) {
		char cursor_buf[1024];
		char *cursor;
		char *serial;
		char *state;
		char *saveptr_inner = NULL;

		_snprintf_s(cursor_buf, sizeof(cursor_buf), _TRUNCATE, "%s", line);
		cursor = cursor_buf;

		while (*cursor == ' ' || *cursor == '\t')
			++cursor;

		if (!cursor[0] || cursor[0] == '\n')
			continue;

		if (!strncmp(cursor, "List of devices attached", 24))
			continue;

		if (cursor[0] == '*')
			continue;

		serial = strtok_s(cursor, " \t\r\n", &saveptr_inner);
		state = strtok_s(NULL, " \t\r\n", &saveptr_inner);
		if (!serial || !state)
			continue;

		if (strcmp(state, "device") != 0)
			continue;

		obs_property_list_add_string(list, serial, serial);
		++found;
	}

	return found;
}

static int scrcpy_parse_adb_devices(FILE *pipe, obs_property_t *list)
{
	char output[4096];
	size_t total = 0;

	while (total < sizeof(output) - 1 && fgets(output + total, (int)(sizeof(output) - total), pipe) != NULL)
		total = strlen(output);
	output[total] = '\0';

	return scrcpy_parse_adb_devices_from_string(output, list);
}

static bool scrcpy_run_adb_command(const char *adb_path, const char *args, char *output, size_t output_size)
{
	char command[1024];
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	HANDLE stdout_read = NULL, stdout_write = NULL;
	SECURITY_ATTRIBUTES sa;
	DWORD wait_result;
	bool ok = false;

	_snprintf_s(command, sizeof(command), _TRUNCATE, "cmd.exe /c \"%s\" %s 2>nul", adb_path, args);

	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	if (output && output_size > 0) {
		if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
			return false;
		SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
	}

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	if (stdout_write)
		si.dwFlags |= STARTF_USESTDHANDLES, si.hStdOutput = stdout_write;

	ZeroMemory(&pi, sizeof(pi));
	if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		if (stdout_write)
			CloseHandle(stdout_write);
		if (stdout_read)
			CloseHandle(stdout_read);
		return false;
	}
	CloseHandle(pi.hThread);

	wait_result = WaitForSingleObject(pi.hProcess, ADB_CMD_TIMEOUT_MS);
	if (wait_result == WAIT_TIMEOUT) {
		TerminateProcess(pi.hProcess, 1);
		obs_log(LOG_WARNING, "scrcpy: adb command timed out after %d ms: %s", ADB_CMD_TIMEOUT_MS, args);
	} else {
		ok = true;
	}

	if (stdout_read) {
		DWORD available = 0, bytes_read = 0;
		size_t total = 0;
		Sleep(30); /* let adb stdout flush */
		while (total < output_size - 1 && PeekNamedPipe(stdout_read, NULL, 0, NULL, &available, NULL) && available > 0) {
			DWORD to_read = (DWORD)(output_size - 1 - total);
			if (to_read > available)
				to_read = available;
			if (!ReadFile(stdout_read, output + total, to_read, &bytes_read, NULL) || bytes_read == 0)
				break;
			total += bytes_read;
		}
		output[total] = '\0';
	}

	if (stdout_write)
		CloseHandle(stdout_write);
	if (stdout_read)
		CloseHandle(stdout_read);
	CloseHandle(pi.hProcess);
	return ok;
}

static int scrcpy_discover_mdns_devices(const char *adb_path)
{
	char output[4096];
	char *cursor;
	int connected = 0;

	if (!scrcpy_run_adb_command(adb_path, "mdns services", output, sizeof(output)))
		return 0;

	/*
	 * Sample output:
	 *   List of discovered mdns services
	 *   adb-<serial>-<token> (N)\t_adb-tls-connect._tcp\t192.168.0.89:39931
	 *   adb-<serial>-<token>\t_adb-tls-connect._tcp\t192.168.0.89:41695
	 *
	 * We only connect to _adb-tls-connect._tcp entries (not _adb-tls-pairing._tcp).
	 * The port is unstable on Android 11+ wireless debugging, so we attempt
	 * adb connect on each discovered address and let adb reject stale ones.
	 */
	cursor = output;
	while (cursor && *cursor) {
		char *line_end = strpbrk(cursor, "\r\n");
		char *next = NULL;

		if (line_end) {
			*line_end = '\0';
			next = line_end + 1;
			while (*next == '\r' || *next == '\n')
				++next;
		}

		if (strstr(cursor, "_adb-tls-connect._tcp")) {
			char *tab = strrchr(cursor, '\t');
			if (tab && strchr(tab + 1, ':')) {
				char connect_args[128];
				_snprintf_s(connect_args, sizeof(connect_args), _TRUNCATE, "connect %s", tab + 1);
				obs_log(LOG_INFO, "scrcpy mdns: attempting adb connect %s", tab + 1);
				if (scrcpy_run_adb_command(adb_path, connect_args, NULL, 0))
					++connected;
			}
		}

		cursor = line_end ? next : NULL;
	}

	return connected;
}

static int scrcpy_refresh_device_list(struct scrcpy_source *context, obs_property_t *list)
{
	const char *adb_path = DEFAULT_ADB_PATH;
	int found = 0;
	char devices_output[4096];

	if (!list)
		return 0;

	if (context && context->adb_path && context->adb_path[0])
		adb_path = context->adb_path;

	obs_property_list_clear(list);
	obs_property_list_add_string(list, "(Select device)", "");

#ifdef _WIN32
	if (!scrcpy_run_adb_command(adb_path, "devices -l", devices_output, sizeof(devices_output))) {
		obs_log(LOG_WARNING, "failed to run adb command using '%s'", adb_path);
		obs_property_list_add_string(list, "ADB command failed", "");
		return 0;
	}

	found = scrcpy_parse_adb_devices_from_string(devices_output, list);

	/*
	 * If no devices were found via adb devices, attempt to discover wireless
	 * debugging devices via mDNS and auto-connect them. On Android 11+ the
	 * wireless debugging port changes on every toggle, so a manual adb connect
	 * is required before the device shows up in the devices list.
	 */
	if (found == 0) {
		obs_log(LOG_INFO, "scrcpy: no devices listed, scanning mDNS for wireless debug devices");
		if (scrcpy_discover_mdns_devices(adb_path) > 0) {
			obs_property_list_clear(list);
			obs_property_list_add_string(list, "(Select device)", "");
			if (scrcpy_run_adb_command(adb_path, "devices -l", devices_output, sizeof(devices_output)))
				found = scrcpy_parse_adb_devices_from_string(devices_output, list);
		}
	}
#else
	UNUSED_PARAMETER(adb_path);
#endif

	if (found == 0)
		obs_property_list_add_string(list, "No online ADB devices", "");

	obs_log(LOG_INFO, "scrcpy discovered %d ADB device(s)", found);
	return found;
}

static bool scrcpy_refresh_button_clicked(obs_properties_t *props, obs_property_t *button, void *data)
{
	UNUSED_PARAMETER(button);

	obs_property_t *device_list = obs_properties_get(props, SETTING_DEVICE_SERIAL);
	scrcpy_refresh_device_list(data, device_list);
	return true;
}

static struct obs_source_info scrcpy_source_info = {
	.id = "scrcpy_camera_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = scrcpy_source_get_name,
	.create = scrcpy_source_create,
	.destroy = scrcpy_source_destroy,
	.activate = scrcpy_source_activate,
	.deactivate = scrcpy_source_deactivate,
	.update = scrcpy_source_update,
	.video_tick = scrcpy_source_tick,
	.get_defaults = scrcpy_source_defaults,
	.get_properties = scrcpy_source_properties,
	.get_width = scrcpy_source_get_width,
	.get_height = scrcpy_source_get_height,
};

void scrcpy_source_register(void)
{
	obs_register_source(&scrcpy_source_info);
}