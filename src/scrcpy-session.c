/*
 * scrcpy session bootstrap
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

#include "scrcpy-session.h"

#include <obs-module.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <plugin-support.h>

#include <util/platform.h>

#include <process.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define DEFAULT_SCRCPY_VERSION "4.0"
#define SCRCPY_META_DEVICE_NAME_SIZE 64
#define SCRCPY_SESSION_PACKET_SIZE 12
#define SCRCPY_FRAME_HEADER_SIZE 12
#define SCRCPY_DEFAULT_PORT 27183
#define SCRCPY_COMMAND_TIMEOUT_MS 15000

struct scrcpy_session {
	char *adb_path;
	char *device_serial;
	char *server_jar_path;
	char *scrcpy_version;
	char *video_codec;
	char *video_source;
	char *camera_id;
	char *camera_size;
	uint16_t local_port;
	uint32_t video_bit_rate;
	uint16_t max_size;
	uint32_t scid;
	bool hw_decoding;
	enum AVHWDeviceType hw_device_type;

	bool audio_enabled;
	char *audio_source;
	char *audio_codec;
	uint32_t audio_bit_rate;
	/* Low-latency level: 0=Off, 1=Low, 2=Medium, 3=High */
	uint8_t low_latency_level;

	scrcpy_session_frame_callback on_frame;
	void *on_frame_opaque;
	scrcpy_session_audio_callback on_audio;
	void *on_audio_opaque;
	uint64_t next_audio_ts;
	char socket_name[64];

	HANDLE worker_thread;
	HANDLE audio_thread;
	HANDLE server_process;
	SOCKET video_socket;
	SOCKET audio_socket;
	volatile LONG stop_requested;
	volatile LONG running;
};

static unsigned __stdcall scrcpy_session_worker(void *opaque);
static bool scrcpy_command_step(struct scrcpy_session *session, const char *step, const char *command_line);
static bool scrcpy_command_fire_and_forget(struct scrcpy_session *session, const char *step, const char *command_line);
static bool scrcpy_run_process(struct scrcpy_session *session, const char *step, const char *command_line,
			       bool wait_for_exit, bool treat_missing_exit_as_success, DWORD *exit_code);
static void scrcpy_log_pipe_output(const char *prefix, HANDLE pipe_handle);
static bool scrcpy_should_stop(struct scrcpy_session *session);
static bool scrcpy_open_video_socket(struct scrcpy_session *session);
static bool scrcpy_open_audio_socket(struct scrcpy_session *session);
static bool scrcpy_read_exact(struct scrcpy_session *session, SOCKET sock, void *buffer, size_t size);
static void scrcpy_log_socket_available(const char *label, SOCKET sock);
static bool scrcpy_read_handshake(struct scrcpy_session *session, enum AVCodecID *codec_id, uint32_t *width,
				  uint32_t *height);
static bool scrcpy_read_audio_handshake(struct scrcpy_session *session, enum AVCodecID *codec_id);
static uint32_t scrcpy_read_be32(const uint8_t *data);
static bool scrcpy_init_decoder(struct scrcpy_session *session, enum AVCodecID codec_id,
				AVCodecContext **decoder_context);
static bool scrcpy_decode_loop(struct scrcpy_session *session, AVCodecContext *decoder_context, uint32_t width,
			       uint32_t height);
static bool scrcpy_audio_decode_loop(struct scrcpy_session *session, AVCodecContext *decoder_context, bool is_raw_pcm);
static unsigned __stdcall scrcpy_audio_worker(void *opaque);
static void scrcpy_close_stream_handles(struct scrcpy_session *session);

#pragma comment(lib, "Ws2_32.lib")

static void scrcpy_copy_config(struct scrcpy_session *session, const struct scrcpy_session_config *config)
{
	bfree(session->adb_path);
	bfree(session->device_serial);
	bfree(session->server_jar_path);
	bfree(session->scrcpy_version);
	bfree(session->video_codec);
	bfree(session->video_source);
	bfree(session->camera_id);
	bfree(session->camera_size);
	bfree(session->audio_source);
	bfree(session->audio_codec);

	session->adb_path = bstrdup(config->adb_path ? config->adb_path : "adb.exe");
	session->device_serial = bstrdup(config->device_serial ? config->device_serial : "");
	session->server_jar_path = bstrdup(config->server_jar_path ? config->server_jar_path : "scrcpy-server.jar");
	session->scrcpy_version = bstrdup(config->scrcpy_version && config->scrcpy_version[0] ? config->scrcpy_version
											      : DEFAULT_SCRCPY_VERSION);
	session->video_codec = bstrdup(config->video_codec && config->video_codec[0] ? config->video_codec : "h264");
	session->video_source =
		bstrdup(config->video_source && config->video_source[0] ? config->video_source : "display");
	session->camera_id = bstrdup(config->camera_id && config->camera_id[0] ? config->camera_id : "0");
	session->camera_size =
		bstrdup(config->camera_size && config->camera_size[0] ? config->camera_size : "1920x1080");
	session->audio_source =
		bstrdup(config->audio_source && config->audio_source[0] ? config->audio_source : "output");
	session->audio_codec = bstrdup(config->audio_codec && config->audio_codec[0] ? config->audio_codec : "opus");
	session->local_port = config->local_port ? config->local_port : SCRCPY_DEFAULT_PORT;
	session->video_bit_rate = config->video_bit_rate ? config->video_bit_rate : 8000000;
	session->audio_bit_rate = config->audio_bit_rate ? config->audio_bit_rate : 128000;
	session->max_size = config->max_size;
	session->hw_decoding = config->hw_decoding;
	session->audio_enabled = config->audio_enabled;
	session->low_latency_level = config->low_latency_level;
	session->scid = (GetCurrentProcessId() ^ GetTickCount()) & 0x7fffffffU;
	session->on_frame = config->on_frame;
	session->on_frame_opaque = config->on_frame_opaque;
	session->on_audio = config->on_audio;
	session->on_audio_opaque = config->on_audio_opaque;
	_snprintf_s(session->socket_name, sizeof(session->socket_name), _TRUNCATE, "scrcpy_%08x", session->scid);
}

struct scrcpy_session *scrcpy_session_create(void)
{
	return bzalloc(sizeof(struct scrcpy_session));
}

void scrcpy_session_destroy(struct scrcpy_session *session)
{
	if (!session)
		return;

	scrcpy_close_stream_handles(session);
	scrcpy_session_stop(session);
	bfree(session->adb_path);
	bfree(session->device_serial);
	bfree(session->server_jar_path);
	bfree(session->scrcpy_version);
	bfree(session->video_codec);
	bfree(session->video_source);
	bfree(session->camera_id);
	bfree(session->camera_size);
	bfree(session->audio_source);
	bfree(session->audio_codec);
	bfree(session);
}

bool scrcpy_session_is_running(const struct scrcpy_session *session)
{
	return session && InterlockedCompareExchange((LONG *)&session->running, 0, 0) != 0;
}

int scrcpy_session_start(struct scrcpy_session *session, const struct scrcpy_session_config *config)
{
	uintptr_t thread_handle;

	if (!session || !config)
		return -1;

	if (!config->device_serial || !config->device_serial[0]) {
		obs_log(LOG_WARNING, "scrcpy session start skipped: no device serial selected");
		return -2;
	}

	scrcpy_session_stop(session);
	scrcpy_copy_config(session, config);

	InterlockedExchange(&session->stop_requested, 0);
	InterlockedExchange(&session->running, 1);

	thread_handle = _beginthreadex(NULL, 0, scrcpy_session_worker, session, 0, NULL);
	if (!thread_handle) {
		InterlockedExchange(&session->running, 0);
		obs_log(LOG_ERROR, "failed to create scrcpy session worker thread");
		return -3;
	}

	session->worker_thread = (HANDLE)thread_handle;
	obs_log(LOG_INFO, "scrcpy session worker started for device '%s'", session->device_serial);
	return 0;
}

void scrcpy_session_stop(struct scrcpy_session *session)
{
	if (!session)
		return;

	InterlockedExchange(&session->stop_requested, 1);
	scrcpy_close_stream_handles(session);

	if (session->audio_thread) {
		WaitForSingleObject(session->audio_thread, INFINITE);
		CloseHandle(session->audio_thread);
		session->audio_thread = NULL;
	}

	if (!session->worker_thread) {
		InterlockedExchange(&session->running, 0);
		return;
	}

	WaitForSingleObject(session->worker_thread, INFINITE);
	CloseHandle(session->worker_thread);
	session->worker_thread = NULL;
	InterlockedExchange(&session->running, 0);
	obs_log(LOG_INFO, "scrcpy session worker stopped");
}

static bool scrcpy_should_stop(struct scrcpy_session *session)
{
	return InterlockedCompareExchange(&session->stop_requested, 0, 0) != 0;
}

static void scrcpy_close_stream_handles(struct scrcpy_session *session)
{
	if (!session)
		return;

	if (session->video_socket != INVALID_SOCKET) {
		shutdown(session->video_socket, SD_BOTH);
		closesocket(session->video_socket);
		session->video_socket = INVALID_SOCKET;
	}

	if (session->audio_socket != INVALID_SOCKET) {
		shutdown(session->audio_socket, SD_BOTH);
		closesocket(session->audio_socket);
		session->audio_socket = INVALID_SOCKET;
	}

	if (session->server_process) {
		TerminateProcess(session->server_process, 0);
		CloseHandle(session->server_process);
		session->server_process = NULL;
	}
}

static bool scrcpy_command_step(struct scrcpy_session *session, const char *step, const char *command)
{
	return scrcpy_run_process(session, step, command, true, false, NULL);
}

static bool scrcpy_command_fire_and_forget(struct scrcpy_session *session, const char *step, const char *command)
{
	return scrcpy_run_process(session, step, command, false, false, NULL);
}

static bool scrcpy_run_process(struct scrcpy_session *session, const char *step, const char *command_line,
			       bool wait_for_exit, bool treat_missing_exit_as_success, DWORD *exit_code)
{
	STARTUPINFOA startup_info;
	PROCESS_INFORMATION process_info;
	HANDLE stdout_read = NULL;
	HANDLE stdout_write = NULL;
	HANDLE stderr_read = NULL;
	HANDLE stderr_write = NULL;
	char buffer[1024];
	char command_buf[4096];
	DWORD process_exit_code = 0;
	uint64_t start_ns = os_gettime_ns();
	uint64_t deadline_ns = start_ns + (uint64_t)SCRCPY_COMMAND_TIMEOUT_MS * 1000000ULL;
	bool success = false;

	if (scrcpy_should_stop(session))
		return false;

	obs_log(LOG_INFO, "scrcpy step: %s", step);

	/* OPT #6: Deduplicate startup_info init — shared fields first */
	ZeroMemory(&startup_info, sizeof(startup_info));
	startup_info.cb = sizeof(startup_info);
	startup_info.dwFlags = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_HIDE;

	if (wait_for_exit) {
		SECURITY_ATTRIBUTES security_attributes;

		ZeroMemory(&security_attributes, sizeof(security_attributes));
		security_attributes.nLength = sizeof(security_attributes);
		security_attributes.bInheritHandle = TRUE;
		security_attributes.lpSecurityDescriptor = NULL;

		if (!CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0)) {
			obs_log(LOG_ERROR, "failed to create stdout pipe for step '%s' (error %lu)", step,
				GetLastError());
			return false;
		}

		if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
			obs_log(LOG_ERROR, "failed to configure stdout pipe for step '%s' (error %lu)", step,
				GetLastError());
			goto done;
		}

		if (!CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0)) {
			obs_log(LOG_ERROR, "failed to create stderr pipe for step '%s' (error %lu)", step,
				GetLastError());
			goto done;
		}

		if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
			obs_log(LOG_ERROR, "failed to configure stderr pipe for step '%s' (error %lu)", step,
				GetLastError());
			goto done;
		}

		startup_info.dwFlags |= STARTF_USESTDHANDLES;
		startup_info.hStdOutput = stdout_write;
		startup_info.hStdError = stderr_write;
		startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	}
	ZeroMemory(&process_info, sizeof(process_info));

	/* OPT #1: Run process directly without cmd.exe wrapper to save ~25-40ms per command */
	_snprintf_s(command_buf, sizeof(command_buf), _TRUNCATE, "%s", command_line);
	obs_log(LOG_DEBUG, "scrcpy command line: %s", command_buf);
	if (!CreateProcessA(NULL, command_buf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup_info,
			    &process_info)) {
		obs_log(LOG_ERROR, "failed to start process for step '%s' (error %lu)", step, GetLastError());
		goto done;
	}

	CloseHandle(process_info.hThread);

	if (wait_for_exit) {
		for (;;) {
			DWORD wait_result = WaitForSingleObject(process_info.hProcess, 100);
			DWORD bytes_available = 0;

			if (stdout_read && PeekNamedPipe(stdout_read, NULL, 0, NULL, &bytes_available, NULL) &&
			    bytes_available > 0) {
				DWORD bytes_read = 0;
				if (ReadFile(stdout_read, buffer, sizeof(buffer) - 1, &bytes_read, NULL) &&
				    bytes_read > 0) {
					buffer[bytes_read] = '\0';
					obs_log(LOG_INFO, "scrcpy adb: %s", buffer);
				}
			}

			bytes_available = 0;
			if (stderr_read && PeekNamedPipe(stderr_read, NULL, 0, NULL, &bytes_available, NULL) &&
			    bytes_available > 0) {
				DWORD bytes_read = 0;
				if (ReadFile(stderr_read, buffer, sizeof(buffer) - 1, &bytes_read, NULL) &&
				    bytes_read > 0) {
					buffer[bytes_read] = '\0';
					obs_log(LOG_INFO, "scrcpy adb: %s", buffer);
				}
			}

			if (wait_result == WAIT_OBJECT_0)
				break;

			if (scrcpy_should_stop(session)) {
				obs_log(LOG_WARNING, "scrcpy command aborted by stop request: %s", step);
				TerminateProcess(process_info.hProcess, 1);
				break;
			}

			if (os_gettime_ns() > deadline_ns) {
				obs_log(LOG_WARNING, "scrcpy command timed out after %u ms: %s",
					SCRCPY_COMMAND_TIMEOUT_MS, step);
				TerminateProcess(process_info.hProcess, 1);
				break;
			}
		}

		if (!GetExitCodeProcess(process_info.hProcess, &process_exit_code)) {
			obs_log(LOG_WARNING, "failed to read exit code for step '%s' (error %lu)", step,
				GetLastError());
			process_exit_code = 1;
		}

		if (exit_code)
			*exit_code = process_exit_code;
		if (process_exit_code != 0) {
			if (treat_missing_exit_as_success && process_exit_code == 1) {
				success = true;
			} else {
				obs_log(LOG_WARNING, "command returned non-zero exit code (%lu): %s", process_exit_code,
					command_line);
				success = false;
			}
		} else {
			success = true;
		}
		obs_log(LOG_DEBUG, "scrcpy step completed in %.3f sec: %s", (double)(os_gettime_ns() - start_ns) / 1e9,
			step);
	} else {
		session->server_process = process_info.hProcess;
		process_info.hProcess = NULL;
		success = true;
		obs_log(LOG_DEBUG, "scrcpy fire-and-forget launched in %.3f sec: %s",
			(double)(os_gettime_ns() - start_ns) / 1e9, step);
		obs_log(LOG_DEBUG, "scrcpy process handle retained for background server step: %s", step);
	}

done:
	if (stdout_write)
		CloseHandle(stdout_write);
	if (stdout_read)
		CloseHandle(stdout_read);
	if (stderr_write)
		CloseHandle(stderr_write);
	if (stderr_read)
		CloseHandle(stderr_read);
	if (process_info.hProcess)
		CloseHandle(process_info.hProcess);
	if (!success && process_info.hProcess == session->server_process) {
		session->server_process = NULL;
	}
	return success;
}

static void scrcpy_log_socket_available(const char *label, SOCKET sock)
{
	unsigned long available = 0;

	if (sock == INVALID_SOCKET)
		return;

	if (ioctlsocket(sock, FIONREAD, &available) == 0) {
		obs_log(LOG_DEBUG, "scrcpy socket state [%s]: %lu byte(s) buffered", label, available);
	} else {
		obs_log(LOG_DEBUG, "scrcpy socket state [%s]: FIONREAD failed (%d)", label, WSAGetLastError());
	}
}

static bool scrcpy_open_video_socket(struct scrcpy_session *session)
{
	SOCKET sock = INVALID_SOCKET;
	struct sockaddr_in addr;
	int timeout_ms = 250;
	int attempt;

	if (!session)
		return false;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(session->local_port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (attempt = 0; attempt < 5; ++attempt) {
		if (scrcpy_should_stop(session))
			return false;

		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
			return false;

		if (connect(sock, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
			/*
			 * Low-latency socket tuning:
			 *   Level 0 (Off):    256 KB rcvbuf, 250 ms timeout
			 *   Level 1 (Low):     64 KB rcvbuf, 100 ms timeout
			 *   Level 2 (Medium):  32 KB rcvbuf,  50 ms timeout
			 *   Level 3 (High):    16 KB rcvbuf,  30 ms timeout
			 * Smaller buffers reduce buffering delay at the cost of
			 * burst tolerance; shorter timeouts improve responsiveness.
			 */
			int rcvbuf;
			int nodelay = 1; /* OPT #3: Disable Nagle for lower latency */
			switch (session->low_latency_level) {
			case 1:
				rcvbuf = 64 * 1024;
				timeout_ms = 100;
				break;
			case 2:
				rcvbuf = 32 * 1024;
				timeout_ms = 50;
				break;
			case 3:
				rcvbuf = 16 * 1024;
				timeout_ms = 30;
				break;
			default: /* Level 0: Off */
				rcvbuf = 256 * 1024; /* OPT #2: 256KB receive buffer for burst absorption */
				break;
			}
			setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
			setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
			setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
			session->video_socket = sock;
			obs_log(LOG_DEBUG, "connected to scrcpy TCP port %hu (low_latency=%d, rcvbuf=%dKB, timeout=%dms)",
				session->local_port, session->low_latency_level, rcvbuf / 1024, timeout_ms);
			scrcpy_log_socket_available("after connect", sock);
			return true;
		}

		closesocket(sock);
		sock = INVALID_SOCKET;
		Sleep(50);
	}

	obs_log(LOG_ERROR, "unable to connect to local scrcpy video socket on tcp:%hu", session->local_port);
	return false;
}

static bool scrcpy_read_exact(struct scrcpy_session *session, SOCKET sock, void *buffer, size_t size)
{
	uint8_t *cursor = buffer;
	size_t remaining = size;

	// obs_log(LOG_INFO, "[DEBUG] scrcpy_read_exact requesting %zu bytes", size);

	while (remaining > 0) {
		int received;
		int last_error;

		if (scrcpy_should_stop(session))
			return false;

		received = recv(sock, (char *)cursor, (int)remaining, 0);
		last_error = WSAGetLastError();
		if (received > 0) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy recv returned %d bytes (remaining: %zu -> %zu)", received, remaining, remaining - received);
			cursor += received;
			remaining -= (size_t)received;
			continue;
		}

		if (received == 0) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy socket closed (0 returned from recv) while waiting for %zu byte(s) out of %zu", remaining, size);
			return false;
		}

		if (last_error == WSAETIMEDOUT) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy recv timed out, retrying... (remaining=%zu)", remaining); // Might spam too much
			continue;
		}

		// obs_log(LOG_INFO, "[DEBUG] scrcpy recv failed: received=%d remaining=%zu error=%d", received, remaining, last_error);

		return false;
	}

	// obs_log(LOG_INFO, "[DEBUG] scrcpy_read_exact successfully read %zu bytes", size);
	return true;
}

static uint32_t scrcpy_read_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static bool scrcpy_read_handshake(struct scrcpy_session *session, enum AVCodecID *codec_id, uint32_t *width,
				  uint32_t *height)
{
	uint8_t dummy = 0;
	char device_name[SCRCPY_META_DEVICE_NAME_SIZE + 1];
	uint8_t codec_bytes[4];
	uint8_t session_packet[SCRCPY_SESSION_PACKET_SIZE];
	uint32_t codec = 0;

	/*
	 * Protocol lock: this parser intentionally targets scrcpy-server 4.0 behavior
	 * over adb forward with video enabled and audio/control disabled.
	 */
	// obs_log(LOG_INFO, "[DEBUG] waiting for scrcpy forward dummy byte");
	scrcpy_log_socket_available("before dummy byte", session->video_socket);
	if (!scrcpy_read_exact(session, session->video_socket, &dummy, sizeof(dummy))) {
		scrcpy_log_socket_available("dummy byte read failed", session->video_socket);
		// obs_log(LOG_INFO, "[DEBUG] failed to read scrcpy forward dummy byte (will retry)");
		return false;
	}
	// obs_log(LOG_INFO, "[DEBUG] scrcpy forward dummy byte value: %u", dummy);

	// obs_log(LOG_INFO, "[DEBUG] waiting for scrcpy 64-byte device metadata");
	if (!scrcpy_read_exact(session, session->video_socket, device_name, SCRCPY_META_DEVICE_NAME_SIZE)) {
		scrcpy_log_socket_available("device metadata read failed", session->video_socket);
		obs_log(LOG_ERROR, "failed to read scrcpy device metadata");
		return false;
	}

	device_name[SCRCPY_META_DEVICE_NAME_SIZE] = '\0';
	for (size_t i = 0; i < SCRCPY_META_DEVICE_NAME_SIZE; ++i) {
		if (device_name[i] == '\0')
			break;
		if (device_name[i] == '\n' || device_name[i] == '\r') {
			device_name[i] = '\0';
			break;
		}
	}
	obs_log(LOG_INFO, "scrcpy device metadata: '%s'", device_name);

	if (!scrcpy_read_exact(session, session->video_socket, codec_bytes, sizeof(codec_bytes))) {
		scrcpy_log_socket_available("codec id read failed", session->video_socket);
		obs_log(LOG_ERROR, "failed to read scrcpy codec id");
		return false;
	}

	codec = scrcpy_read_be32(codec_bytes);
	if (codec == 0x68323634U) {
		*codec_id = AV_CODEC_ID_H264;
		obs_log(LOG_INFO, "scrcpy codec id: h264");
	} else if (codec == 0x68323635U) {
		*codec_id = AV_CODEC_ID_HEVC;
		obs_log(LOG_INFO, "scrcpy codec id: h265");
	} else {
		obs_log(LOG_ERROR, "unsupported scrcpy codec id: 0x%08x", codec);
		return false;
	}

	/*
	 * Display mirroring sends a 0x80 session packet with initial
	 * dimensions before video frames. Camera mode does NOT — the
	 * stream goes straight to codec config / video frames.
	 */
	if (strcmp(session->video_source, "camera") == 0) {
		obs_log(LOG_INFO, "scrcpy camera mode: skipping session packet, dimensions from decoder");
		*width = 0;
		*height = 0;
	} else {
		if (!scrcpy_read_exact(session, session->video_socket, session_packet, sizeof(session_packet))) {
			scrcpy_log_socket_available("session packet read failed", session->video_socket);
			obs_log(LOG_ERROR, "failed to read scrcpy video session packet");
			return false;
		}
		if ((session_packet[0] & 0x80U) == 0) {
			obs_log(LOG_ERROR, "expected 0x80 session packet, got 0x%02x", session_packet[0]);
			return false;
		}
		*width = scrcpy_read_be32(session_packet + 4);
		*height = scrcpy_read_be32(session_packet + 8);
		obs_log(LOG_INFO, "scrcpy session dimensions: %ux%u", *width, *height);
	}
	return true;
}

static enum AVPixelFormat scrcpy_get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	struct scrcpy_session *session = ctx->opaque;
	const enum AVPixelFormat *p;
	char fmts_str[256] = {0};
	for (p = pix_fmts; *p != -1; p++) {
		const char *name = av_get_pix_fmt_name(*p);
		if (name) {
			strncat(fmts_str, name, sizeof(fmts_str) - strlen(fmts_str) - 1);
			strncat(fmts_str, ", ", sizeof(fmts_str) - strlen(fmts_str) - 1);

			if (session) {
				if (session->hw_device_type == AV_HWDEVICE_TYPE_D3D11VA) {
					if (strcmp(name, "d3d11") == 0 || strcmp(name, "d3d11va_vld") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				} else if (session->hw_device_type == AV_HWDEVICE_TYPE_DXVA2) {
					if (strcmp(name, "dxva2_vld") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				} else if (session->hw_device_type == AV_HWDEVICE_TYPE_CUDA) {
					if (strcmp(name, "cuda") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				} else if (session->hw_device_type == AV_HWDEVICE_TYPE_QSV) {
					if (strcmp(name, "qsv") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				}
			}
		}
	}
	// We only log this once to avoid spamming
	static bool warned = false;
	if (!warned) {
		obs_log(LOG_WARNING,
			"failed to get HW surface format, available formats: %s. Hardware decoding will not be used",
			fmts_str);
		warned = true;
	}
	return pix_fmts[0];
}

static bool scrcpy_init_decoder(struct scrcpy_session *session, enum AVCodecID codec_id,
				AVCodecContext **decoder_context)
{
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	AVCodecContext *context;
	AVBufferRef *hw_device_ctx = NULL;

	if (!codec) {
		obs_log(LOG_ERROR, "ffmpeg decoder not found for codec id %d", (int)codec_id);
		return false;
	}

	session->hw_device_type = AV_HWDEVICE_TYPE_NONE;

	if (session->hw_decoding) {
		const char *hw_types[] = {"cuda", "qsv", "d3d11va", "dxva2", NULL};
		enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;

		for (int i = 0; hw_types[i]; i++) {
			hw_type = av_hwdevice_find_type_by_name(hw_types[i]);
			if (hw_type != AV_HWDEVICE_TYPE_NONE) {
				int err = av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0);
				if (err == 0) {
					obs_log(LOG_INFO, "hardware decoding context created successfully: %s",
						hw_types[i]);
					session->hw_device_type = hw_type;
					break;
				}
			}
		}

		if (!hw_device_ctx) {
			obs_log(LOG_WARNING, "failed to create any hardware device context, falling back to software");
		}
	}

	context = avcodec_alloc_context3(codec);
	if (!context) {
		obs_log(LOG_ERROR, "failed to allocate ffmpeg decoder context");
		if (hw_device_ctx)
			av_buffer_unref(&hw_device_ctx);
		return false;
	}

	if (hw_device_ctx) {
		context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
		context->opaque = session;
		context->get_format = scrcpy_get_hw_format;
		av_buffer_unref(&hw_device_ctx);
	}

	/* OPT #7: Enable slice-threading for software decode to parallelize
	 * decoding within a single frame without adding latency.
	 * FF_THREAD_SLICE splits each frame's slices across threads — no extra
	 * buffering needed, unlike FF_THREAD_FRAME which delays output by N frames.
	 * HW decoders manage parallelism internally via GPU, so keep thread_count=1. */
	if (context->hw_device_ctx) {
		context->thread_count = 1;
	} else {
		context->thread_count = 0; /* 0 = auto-detect (typically logical core count) */
		context->thread_type = FF_THREAD_SLICE;
		obs_log(LOG_INFO, "software decode: slice-threading enabled (auto thread count)");
	}

	/*
	 * Low-latency decoder flag tuning:
	 *   Level >= 1: AV_CODEC_FLAG_LOW_DELAY — disables decoder output reordering
	 *               so frames are emitted as soon as they're decoded.
	 *   Level >= 2: AV_CODEC_FLAG2_FAST — enables speed optimizations that may
	 *               not conform to the spec but reduce decode time.
	 *   Level >= 3: skip_loop_filter = AVDISCARD_ALL — skips the in-loop
	 *               deblocking filter entirely; saves ~1 frame of decode time
	 *               at the cost of slight edge blockiness.
	 */
	if (session->low_latency_level >= 1) {
		context->flags |= AV_CODEC_FLAG_LOW_DELAY;
		obs_log(LOG_INFO, "low-latency: decoder flag AV_CODEC_FLAG_LOW_DELAY enabled");
	}
	if (session->low_latency_level >= 2) {
		context->flags2 |= AV_CODEC_FLAG2_FAST;
		obs_log(LOG_INFO, "low-latency: decoder flag AV_CODEC_FLAG2_FAST enabled");
	}
	if (session->low_latency_level >= 3) {
		context->skip_loop_filter = AVDISCARD_ALL;
		obs_log(LOG_INFO, "low-latency: skip_loop_filter set to AVDISCARD_ALL");
	}

	if (avcodec_open2(context, codec, NULL) < 0) {
		obs_log(LOG_ERROR, "failed to open ffmpeg decoder");
		avcodec_free_context(&context);
		return false;
	}

	// obs_log(LOG_INFO, "[DEBUG] ffmpeg decoder initialized successfully (hw_decoding=%d, threads=%d)", session->hw_decoding, context->thread_count);

	*decoder_context = context;
	return true;
}

static bool scrcpy_decode_loop(struct scrcpy_session *session, AVCodecContext *decoder_context, uint32_t width,
			       uint32_t height)
{
	uint8_t header[SCRCPY_FRAME_HEADER_SIZE];
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	/* OPT #4: Pre-allocate sw_frame once to avoid heap alloc/free per HW decoded frame */
	AVFrame *sw_frame = av_frame_alloc();
	bool warned_format = false;
	uint8_t *codec_config = NULL;
	size_t codec_config_size = 0;

	/* OPT #8: Pre-allocate a persistent read buffer to avoid av_new_packet malloc/free
	 * per frame. The buffer auto-grows for large payloads and is reused across frames.
	 * After avcodec_send_packet returns, the decoder holds its own copy of the data,
	 * so our buffer is safe to reuse immediately. */
	size_t read_buf_capacity = 256 * 1024;
	uint8_t *read_buf = av_malloc(read_buf_capacity + AV_INPUT_BUFFER_PADDING_SIZE);

	if (!packet || !frame || !sw_frame || !read_buf) {
		obs_log(LOG_ERROR, "unable to allocate ffmpeg packet/frame objects");
		av_packet_free(&packet);
		av_frame_free(&frame);
		av_frame_free(&sw_frame);
		av_free(read_buf);
		return false;
	}

	obs_log(LOG_INFO, "scrcpy decode loop starting (socket=%lld)", (long long)session->video_socket);

	while (!scrcpy_should_stop(session)) {
		uint32_t payload_size;
		bool is_session_packet;
		int send_ret;

		if (!scrcpy_read_exact(session, session->video_socket, header, sizeof(header))) {
			obs_log(LOG_WARNING, "scrcpy decode loop: header read failed (WSA=%d)", WSAGetLastError());
			break;
		}

		is_session_packet = (header[0] & 0x80U) != 0;
		if (is_session_packet) {
			width = scrcpy_read_be32(header + 4);
			height = scrcpy_read_be32(header + 8);
			obs_log(LOG_INFO, "scrcpy session refresh: %ux%u", width, height);
			continue;
		}

		payload_size = scrcpy_read_be32(header + 8);
		if (payload_size == 0)
			continue;

		/* OPT #8: Grow the persistent read buffer if needed (rare, only on resolution increase) */
		if (payload_size > read_buf_capacity) {
			av_free(read_buf);
			read_buf_capacity = (size_t)payload_size * 2;
			read_buf = av_malloc(read_buf_capacity + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!read_buf) {
				obs_log(LOG_ERROR, "scrcpy decode loop: failed to grow read buffer to %zu",
					read_buf_capacity);
				break;
			}
		}

		if (!scrcpy_read_exact(session, session->video_socket, read_buf, payload_size)) {
			obs_log(LOG_WARNING, "scrcpy decode loop: failed to read payload (%u bytes)", payload_size);
			break;
		}

		if ((header[0] & 0x20U) != 0)
			packet->flags |= AV_PKT_FLAG_KEY;

		/*
		 * Codec config packets (flag 0x40) carry H.264 SPS/PPS
		 * parameter sets without picture data. Store them and
		 * prepend to the first keyframe so the decoder receives
		 * a complete access unit (SPS + PPS + IDR slice).
		 */
		if ((header[0] & 0x40U) != 0) {
			av_free(codec_config);
			codec_config = av_malloc(payload_size);
			if (!codec_config) {
				obs_log(LOG_ERROR, "scrcpy decode loop: failed to allocate codec config buffer");
				break;
			}
			memcpy(codec_config, read_buf, payload_size);
			codec_config_size = payload_size;
			packet->flags = 0;
			continue;
		}

		/*
		 * OPT #8: If we have stored codec config and this is a keyframe,
		 * prepend the config data within read_buf using memmove to avoid
		 * allocating a separate combined AVPacket.
		 */
		if (codec_config && (header[0] & 0x20U) != 0) {
			size_t combined_size = codec_config_size + payload_size;

			/* Grow buffer if combined data doesn't fit */
			if (combined_size > read_buf_capacity) {
				uint8_t *new_buf;
				read_buf_capacity = combined_size * 2;
				new_buf = av_malloc(read_buf_capacity + AV_INPUT_BUFFER_PADDING_SIZE);
				if (!new_buf) {
					obs_log(LOG_ERROR, "scrcpy decode loop: failed to grow read buffer for "
							   "codec config prepend (%zu bytes)",
						combined_size);
					break;
				}
				/* Copy payload into new buffer after codec config position */
				memcpy(new_buf + codec_config_size, read_buf, payload_size);
				av_free(read_buf);
				read_buf = new_buf;
			} else {
				/* Shift payload forward to make room for codec config at the front */
				memmove(read_buf + codec_config_size, read_buf, payload_size);
			}

			/* Prepend codec config (SPS/PPS) before the keyframe data */
			memcpy(read_buf, codec_config, codec_config_size);
			payload_size = (uint32_t)combined_size;

			av_free(codec_config);
			codec_config = NULL;
			codec_config_size = 0;
		}

		/* OPT #8: Zero-copy packet setup — point directly at read_buf.
		 * avcodec_send_packet copies data internally, so read_buf is safe to reuse.
		 * Padding bytes are required by FFmpeg decoders for SIMD overread safety. */
		memset(read_buf + payload_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		packet->data = read_buf;
		packet->size = (int)payload_size;

		send_ret = avcodec_send_packet(decoder_context, packet);
		/* Reset packet without freeing — we own read_buf, not the packet */
		packet->data = NULL;
		packet->size = 0;
		packet->flags = 0;
		if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
			obs_log(LOG_ERROR, "scrcpy decode loop: avcodec_send_packet failed (%d)", send_ret);
			break;
		}

		for (;;) {
			int recv_ret = avcodec_receive_frame(decoder_context, frame);
			if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
				break;
			}
			if (recv_ret < 0) {
				obs_log(LOG_ERROR, "[DEBUG] scrcpy decode loop: avcodec_receive_frame failed (%d)",
					recv_ret);
				av_frame_unref(frame);
				goto fail;
			}

			/* OPT #4: Reuse pre-allocated sw_frame instead of alloc/free per HW frame.
			 * av_frame_unref() releases internal pixel buffers each iteration
			 * while keeping the AVFrame struct alive — no memory leak. */
			AVFrame *output_frame = frame;
			bool used_sw_frame = false;

			if (frame->format != AV_PIX_FMT_YUV420P && frame->hw_frames_ctx) {
				av_frame_unref(sw_frame);
				if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
					obs_log(LOG_ERROR, "scrcpy decode loop: failed to transfer hw frame to sw");
					av_frame_unref(frame);
					break;
				}
				output_frame = sw_frame;
				used_sw_frame = true;
			}

			if ((output_frame->format == AV_PIX_FMT_YUV420P || output_frame->format == AV_PIX_FMT_NV12) &&
			    session->on_frame) {
				struct obs_source_frame obs_frame;
				memset(&obs_frame, 0, sizeof(obs_frame));

				if (output_frame->format == AV_PIX_FMT_NV12) {
					obs_frame.format = VIDEO_FORMAT_NV12;
					obs_frame.data[0] = output_frame->data[0];
					obs_frame.data[1] = output_frame->data[1];
					obs_frame.linesize[0] = output_frame->linesize[0];
					obs_frame.linesize[1] = output_frame->linesize[1];
				} else {
					obs_frame.format = VIDEO_FORMAT_I420;
					obs_frame.data[0] = output_frame->data[0];
					obs_frame.data[1] = output_frame->data[1];
					obs_frame.data[2] = output_frame->data[2];
					obs_frame.linesize[0] = output_frame->linesize[0];
					obs_frame.linesize[1] = output_frame->linesize[1];
					obs_frame.linesize[2] = output_frame->linesize[2];
				}

				obs_frame.width = output_frame->width;
				obs_frame.height = output_frame->height;
				obs_frame.timestamp = os_gettime_ns();

				video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
								       obs_frame.format, obs_frame.color_matrix,
								       obs_frame.color_range_min,
								       obs_frame.color_range_max);
				session->on_frame(session->on_frame_opaque, &obs_frame);
			} else if (!warned_format && session->on_frame) {
				obs_log(LOG_WARNING,
					"decoder output format %d is not AV_PIX_FMT_YUV420P or NV12; frame dropped without swscale",
					output_frame->format);
				warned_format = true;
			}

			if (used_sw_frame)
				av_frame_unref(sw_frame);
			av_frame_unref(frame);
		}
	}

	obs_log(LOG_DEBUG, "scrcpy decode loop exiting (stop_requested=%ld)",
		InterlockedCompareExchange(&session->stop_requested, 0, 0));
	av_free(codec_config);
	av_free(read_buf);
	av_frame_unref(frame);
	av_frame_unref(sw_frame);
	av_packet_free(&packet);
	av_frame_free(&frame);
	av_frame_free(&sw_frame);
	return true;

fail:
	av_free(codec_config);
	av_free(read_buf);
	av_frame_unref(frame);
	av_frame_unref(sw_frame);
	av_packet_free(&packet);
	av_frame_free(&frame);
	av_frame_free(&sw_frame);
	return false;
}

static bool scrcpy_open_audio_socket(struct scrcpy_session *session)
{
	SOCKET sock = INVALID_SOCKET;
	struct sockaddr_in addr;
	int timeout_ms = 250;
	int attempt;

	if (!session)
		return false;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(session->local_port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (attempt = 0; attempt < 10; ++attempt) {
		if (scrcpy_should_stop(session))
			return false;

		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
			return false;

		if (connect(sock, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
			/* Low-latency audio socket tuning — same tiers as video socket */
			int rcvbuf;
			int nodelay = 1; /* OPT #3: Disable Nagle */
			switch (session->low_latency_level) {
			case 1:
				rcvbuf = 64 * 1024;
				timeout_ms = 100;
				break;
			case 2:
				rcvbuf = 32 * 1024;
				timeout_ms = 50;
				break;
			case 3:
				rcvbuf = 16 * 1024;
				timeout_ms = 30;
				break;
			default: /* Level 0: Off */
				rcvbuf = 256 * 1024; /* OPT #2: 256KB receive buffer */
				break;
			}
			setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
			setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
			setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
			session->audio_socket = sock;
			obs_log(LOG_DEBUG, "connected to scrcpy audio TCP port %hu (low_latency=%d)",
				session->local_port, session->low_latency_level);
			return true;
		}

		closesocket(sock);
		sock = INVALID_SOCKET;
		Sleep(100);
	}

	obs_log(LOG_ERROR, "unable to connect to local scrcpy audio socket on tcp:%hu", session->local_port);
	return false;
}

static bool scrcpy_read_audio_handshake(struct scrcpy_session *session, enum AVCodecID *codec_id)
{
	uint8_t codec_bytes[4];
	uint32_t codec;

	if (!scrcpy_read_exact(session, session->audio_socket, codec_bytes, sizeof(codec_bytes))) {
		obs_log(LOG_ERROR, "failed to read scrcpy audio codec id");
		return false;
	}

	codec = scrcpy_read_be32(codec_bytes);
	if (codec == 0x6f707573U) {
		*codec_id = AV_CODEC_ID_OPUS;
		obs_log(LOG_INFO, "scrcpy audio codec: opus");
	} else if (codec == 0x00616163U) {
		*codec_id = AV_CODEC_ID_AAC;
		obs_log(LOG_INFO, "scrcpy audio codec: aac");
	} else if (codec == 0x00726177U) {
		*codec_id = AV_CODEC_ID_NONE; /* raw PCM */
		obs_log(LOG_INFO, "scrcpy audio codec: raw PCM");
	} else if (codec == 0x666c6163U) {
		*codec_id = AV_CODEC_ID_FLAC;
		obs_log(LOG_INFO, "scrcpy audio codec: flac");
	} else {
		obs_log(LOG_ERROR, "unsupported scrcpy audio codec id: 0x%08x", codec);
		return false;
	}

	return true;
}

static bool scrcpy_audio_decode_loop(struct scrcpy_session *session, AVCodecContext *decoder_context, bool is_raw_pcm)
{
	uint8_t header[SCRCPY_FRAME_HEADER_SIZE];
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = is_raw_pcm ? NULL : av_frame_alloc();
	SwrContext *swr_ctx = NULL;
	uint8_t *resample_buf = NULL;
	int resample_buf_size = 0;

	if (!packet || (!is_raw_pcm && !frame)) {
		obs_log(LOG_ERROR, "unable to allocate audio ffmpeg objects");
		av_packet_free(&packet);
		av_frame_free(&frame);
		return false;
	}

	obs_log(LOG_INFO, "scrcpy audio decode loop starting (socket=%lld, raw_pcm=%d)",
		(long long)session->audio_socket, is_raw_pcm);

	while (!scrcpy_should_stop(session)) {
		uint32_t payload_size;

		if (!scrcpy_read_exact(session, session->audio_socket, header, sizeof(header))) {
			obs_log(LOG_WARNING, "scrcpy audio decode loop: header read failed (WSA=%d)",
				WSAGetLastError());
			break;
		}

		payload_size = scrcpy_read_be32(header + 8);
		if (payload_size == 0)
			continue;

		/*
		 * Flag bit layout (same as video):
		 * bit 7 (0x80) of header[0]: not used for audio (no session packet)
		 * bit 6 (0x40) of header[0]: codec config
		 * bit 5 (0x20) of header[0]: key frame / end of stream
		 */

		av_packet_unref(packet);
		if (av_new_packet(packet, (int)payload_size) < 0) {
			obs_log(LOG_ERROR, "scrcpy audio decode loop: av_new_packet failed (%u)", payload_size);
			break;
		}

		if (!scrcpy_read_exact(session, session->audio_socket, packet->data, payload_size)) {
			obs_log(LOG_WARNING, "scrcpy audio decode loop: payload read failed (%u bytes)", payload_size);
			av_packet_unref(packet);
			break;
		}

		/* Skip codec config packets for audio */
		if ((header[0] & 0x40U) != 0) {
			av_packet_unref(packet);
			continue;
		}

		if (is_raw_pcm) {
			/*
			 * Raw PCM from scrcpy: 16-bit signed LE, stereo, 48kHz.
			 * Deliver directly to OBS.
			 */
			struct obs_source_audio obs_audio;
			memset(&obs_audio, 0, sizeof(obs_audio));
			obs_audio.data[0] = packet->data;
			obs_audio.frames = payload_size / (2 * 2); /* 2 bytes/sample * 2 channels */
			obs_audio.speakers = SPEAKERS_STEREO;
			obs_audio.format = AUDIO_FORMAT_16BIT;
			obs_audio.samples_per_sec = 48000;
			/*
			 * Audio drift guard window — determines how far the
			 * monotonic audio timestamp can drift from wall clock
			 * before resetting. Tighter windows at higher low-latency
			 * levels keep audio more closely synced to real-time.
			 *   Level 0: 100 ms, Level 1: 50 ms,
			 *   Level 2: 30 ms, Level 3: 20 ms
			 */
			uint64_t audio_drift_ns;
			switch (session->low_latency_level) {
			case 1:  audio_drift_ns = 50000000ULL;  break; /* 50 ms */
			case 2:  audio_drift_ns = 30000000ULL;  break; /* 30 ms */
			case 3:  audio_drift_ns = 20000000ULL;  break; /* 20 ms */
			default: audio_drift_ns = 100000000ULL; break; /* 100 ms */
			}
			uint64_t now = os_gettime_ns();
			if (session->next_audio_ts == 0 || now > session->next_audio_ts + audio_drift_ns ||
			    now < session->next_audio_ts - audio_drift_ns)
				session->next_audio_ts = now;

			obs_audio.timestamp = session->next_audio_ts;
			session->next_audio_ts +=
				(uint64_t)obs_audio.frames * 1000000000ULL / obs_audio.samples_per_sec;

			if (session->on_audio)
				session->on_audio(session->on_audio_opaque, &obs_audio);
			av_packet_unref(packet);
			continue;
		}

		/* Decode compressed audio (Opus, AAC, FLAC) */
		int send_ret = avcodec_send_packet(decoder_context, packet);
		av_packet_unref(packet);
		if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
			obs_log(LOG_ERROR, "scrcpy audio decode loop: avcodec_send_packet failed (%d)", send_ret);
			break;
		}

		for (;;) {
			int recv_ret = avcodec_receive_frame(decoder_context, frame);
			if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF)
				break;
			if (recv_ret < 0) {
				obs_log(LOG_ERROR, "scrcpy audio decode loop: avcodec_receive_frame failed (%d)",
					recv_ret);
				av_frame_unref(frame);
				goto fail;
			}

			/*
			 * OBS expects interleaved float audio. Use swresample to convert
			 * from whatever the decoder outputs (commonly float planar for Opus,
			 * or float/s16 for AAC) to interleaved float stereo at the decoder's
			 * sample rate.
			 */
			if (!swr_ctx) {
				AVChannelLayout out_layout;
				AVChannelLayout in_layout;

				memset(&out_layout, 0, sizeof(out_layout));
				memset(&in_layout, 0, sizeof(in_layout));
				av_channel_layout_default(&out_layout, 2);

				if (frame->ch_layout.nb_channels > 0) {
					av_channel_layout_copy(&in_layout, &frame->ch_layout);
				} else {
					av_channel_layout_default(&in_layout, 2);
				}

				swr_alloc_set_opts2(&swr_ctx, &out_layout, AV_SAMPLE_FMT_FLT, frame->sample_rate,
						    &in_layout, frame->format, frame->sample_rate, 0, NULL);
				av_channel_layout_uninit(&out_layout);
				av_channel_layout_uninit(&in_layout);
				if (!swr_ctx || swr_init(swr_ctx) < 0) {
					obs_log(LOG_ERROR, "scrcpy audio: failed to init swresample context");
					av_frame_unref(frame);
					goto fail;
				}
				obs_log(LOG_INFO, "scrcpy audio swresample initialized: fmt=%d->FLT, rate=%d, ch=%d",
					frame->format, frame->sample_rate, frame->ch_layout.nb_channels);
			}

			int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
			int needed_size = out_samples * 2 * sizeof(float); /* stereo float */
			if (needed_size > resample_buf_size) {
				av_free(resample_buf);
				resample_buf = av_malloc(needed_size);
				resample_buf_size = needed_size;
				if (!resample_buf) {
					obs_log(LOG_ERROR, "scrcpy audio: failed to allocate resample buffer");
					av_frame_unref(frame);
					goto fail;
				}
			}

			uint8_t *out_buf = resample_buf;
			int converted = swr_convert(swr_ctx, &out_buf, out_samples, (const uint8_t **)frame->data,
						    frame->nb_samples);
			if (converted > 0) {
				struct obs_source_audio obs_audio;
				memset(&obs_audio, 0, sizeof(obs_audio));
				obs_audio.data[0] = resample_buf;
				obs_audio.frames = (uint32_t)converted;
				obs_audio.speakers = SPEAKERS_STEREO;
				obs_audio.format = AUDIO_FORMAT_FLOAT;
				obs_audio.samples_per_sec = frame->sample_rate;
				/* Audio drift guard — same level-based window as raw PCM path */
				uint64_t audio_drift_ns;
				switch (session->low_latency_level) {
				case 1:  audio_drift_ns = 50000000ULL;  break; /* 50 ms */
				case 2:  audio_drift_ns = 30000000ULL;  break; /* 30 ms */
				case 3:  audio_drift_ns = 20000000ULL;  break; /* 20 ms */
				default: audio_drift_ns = 100000000ULL; break; /* 100 ms */
				}
				uint64_t now = os_gettime_ns();
				if (session->next_audio_ts == 0 || now > session->next_audio_ts + audio_drift_ns ||
				    now < session->next_audio_ts - audio_drift_ns)
					session->next_audio_ts = now;

				obs_audio.timestamp = session->next_audio_ts;
				session->next_audio_ts +=
					(uint64_t)obs_audio.frames * 1000000000ULL / obs_audio.samples_per_sec;

				if (session->on_audio)
					session->on_audio(session->on_audio_opaque, &obs_audio);
			}

			av_frame_unref(frame);
		}
	}

	obs_log(LOG_DEBUG, "scrcpy audio decode loop exiting");
	av_free(resample_buf);
	if (swr_ctx)
		swr_free(&swr_ctx);
	av_packet_unref(packet);
	if (frame)
		av_frame_unref(frame);
	av_packet_free(&packet);
	av_frame_free(&frame);
	return true;

fail:
	av_free(resample_buf);
	if (swr_ctx)
		swr_free(&swr_ctx);
	av_packet_unref(packet);
	if (frame)
		av_frame_unref(frame);
	av_packet_free(&packet);
	av_frame_free(&frame);
	return false;
}

static unsigned __stdcall scrcpy_audio_worker(void *opaque)
{
	struct scrcpy_session *session = opaque;
	enum AVCodecID audio_codec_id = AV_CODEC_ID_NONE;
	AVCodecContext *audio_decoder = NULL;
	bool is_raw_pcm = false;

	if (!session || session->audio_socket == INVALID_SOCKET)
		return 0;

	if (!scrcpy_read_audio_handshake(session, &audio_codec_id)) {
		obs_log(LOG_ERROR, "scrcpy audio handshake failed");
		return 0;
	}

	is_raw_pcm = (audio_codec_id == AV_CODEC_ID_NONE);

	if (!is_raw_pcm) {
		const AVCodec *codec = avcodec_find_decoder(audio_codec_id);
		if (!codec) {
			obs_log(LOG_ERROR, "ffmpeg audio decoder not found for codec id %d", (int)audio_codec_id);
			return 0;
		}

		audio_decoder = avcodec_alloc_context3(codec);
		if (!audio_decoder) {
			obs_log(LOG_ERROR, "failed to allocate audio decoder context");
			return 0;
		}

		audio_decoder->thread_count = 1;

		if (avcodec_open2(audio_decoder, codec, NULL) < 0) {
			obs_log(LOG_ERROR, "failed to open audio decoder");
			avcodec_free_context(&audio_decoder);
			return 0;
		}
		obs_log(LOG_INFO, "scrcpy audio decoder initialized");
	}

	scrcpy_audio_decode_loop(session, audio_decoder, is_raw_pcm);

	if (audio_decoder)
		avcodec_free_context(&audio_decoder);

	obs_log(LOG_INFO, "scrcpy audio worker finished");
	return 0;
}

static unsigned __stdcall scrcpy_session_worker(void *opaque)
{
	struct scrcpy_session *session = opaque;
	char command[2048];
	enum AVCodecID codec_id = AV_CODEC_ID_NONE;
	AVCodecContext *decoder_context = NULL;
	uint32_t width = 0;
	uint32_t height = 0;
	WSADATA wsadata;
	int reconnect_attempts = 0;

	if (!session)
		return 0;

	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
		obs_log(LOG_ERROR, "WSAStartup failed");
		InterlockedExchange(&session->running, 0);
		return 0;
	}

	while (!scrcpy_should_stop(session)) {
		session->video_socket = INVALID_SOCKET;
		session->audio_socket = INVALID_SOCKET;
		session->server_process = NULL;
		session->next_audio_ts = 0;
		codec_id = AV_CODEC_ID_NONE;
		decoder_context = NULL;
		width = 0;
		height = 0;

		_snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" start-server", session->adb_path);
		if (!scrcpy_command_step(session, "Start ADB server", command))
			goto cleanup_winsock;

		_snprintf_s(command, sizeof(command), _TRUNCATE,
			    "\"%s\" -s %s push \"%s\" /data/local/tmp/scrcpy-server.jar", session->adb_path,
			    session->device_serial, session->server_jar_path);
		if (!scrcpy_command_step(session, "Push scrcpy server", command))
			goto cleanup_winsock;

		_snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" -s %s forward tcp:%hu localabstract:%s",
			    session->adb_path, session->device_serial, session->local_port, session->socket_name);
		if (!scrcpy_command_step(session, "Configure adb forward", command))
			goto cleanup_winsock;

		_snprintf_s(command, sizeof(command), _TRUNCATE,
			    "\"%s\" -s %s shell CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / "
			    "com.genymobile.scrcpy.Server %s scid=%08x tunnel_forward=true audio=%s control=false "
			    "video_codec=%s",
			    session->adb_path, session->device_serial, session->scrcpy_version, session->scid,
			    session->audio_enabled ? "true" : "false", session->video_codec);

		{
			size_t len = strlen(command);

			/*
			 * Low-latency bitrate capping:
			 *   Level 1 (Low):    cap at 4 Mbps — faster encode, less data in flight
			 *   Level 2 (Medium): cap at 2 Mbps
			 *   Level 3 (High):   cap at 2 Mbps
			 * Only caps if the user's chosen bitrate exceeds the limit.
			 */
			uint32_t effective_bit_rate = session->video_bit_rate;
			if (session->low_latency_level >= 2 && effective_bit_rate > 2000000)
				effective_bit_rate = 2000000;
			else if (session->low_latency_level == 1 && effective_bit_rate > 4000000)
				effective_bit_rate = 4000000;

			if (effective_bit_rate != 8000000) {
				len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
						   " video_bit_rate=%u", effective_bit_rate);
			}

			uint16_t max_size = session->max_size;
			bool has_camera_size = (strcmp(session->video_source, "camera") == 0 && session->camera_size &&
						session->camera_size[0]);

			if (strcmp(session->video_source, "camera") == 0 && !has_camera_size && max_size == 0) {
				/*
			 * Default to 1920 in camera mode if no limit is set. Cameras often support extreme
			 * raw resolutions (e.g. 4000x3000) which the device's hardware H.264 encoder cannot
			 * handle, leading to "Camera configuration error" on startup.
			 */
				max_size = 1920;
			}

			if (max_size > 0 && !has_camera_size) {
				len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE, " max_size=%hu",
						   max_size);
			}
			if (strcmp(session->video_source, "camera") == 0) {
				len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
						   " video_source=camera camera_id=%s", session->camera_id);
				if (has_camera_size) {
					len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
							   " camera_size=%s", session->camera_size);
				}
			}
if (session->audio_enabled) {
				len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
						   " audio_source=%s audio_codec=%s", session->audio_source,
						   session->audio_codec);
				/*
				 * scrcpy playback source defaults to ROUTE_FLAG_LOOP_BACK, which
				 * silences the device. To match the UI label "Playback
				 * (Android 13+ Duplication)", force audio_dup=true so the server
				 * uses ROUTE_FLAG_LOOP_BACK_RENDER and keeps playing on device.
				 */
				if (strcmp(session->audio_source, "playback") == 0) {
					len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
							   " audio_dup=true");
				}
				if (session->audio_bit_rate != 128000) {
					len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
						   " audio_bit_rate=%u", session->audio_bit_rate);
				}
			}

			/*
			 * Low-latency I-frame interval:
			 * At level 2+, force every frame to be an I-frame (no P-frames).
			 * This eliminates decode-pipeline latency from reference frames
			 * but significantly increases bandwidth usage.
			 */
			if (session->low_latency_level >= 2) {
				len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
						   " i_frame_interval=1");
				obs_log(LOG_INFO, "low-latency: i_frame_interval=1 appended to server command");
			}
		}

		obs_log(LOG_INFO, "scrcpy server command: ...%s",
			strlen(command) > 80 ? command + strlen(command) - 80 : command);
		if (!scrcpy_command_fire_and_forget(session, "Start scrcpy app_process server", command))
			goto cleanup_winsock;

		/*
	 * The scrcpy server takes some time to start on the device after
	 * fire-and-forget launch. connect() may succeed immediately because
	 * adb forward is already listening, but the server hasn't yet bound
	 * the abstract socket. When that happens, adb closes the forwarded
	 * connection and the dummy byte read returns EOF. Retry the full
	 * connect+handshake sequence to account for this race.
	 */
		{
			int max_attempts = 15;
			int connect_attempt;
			bool handshake_ok = false;

			/* Camera startup is slower: Camera2 API init, capture session
		 * setup, and encoder configuration can take several seconds. */
			if (strcmp(session->video_source, "camera") == 0) {
				max_attempts = 25;
			}

			for (connect_attempt = 0; connect_attempt < max_attempts; ++connect_attempt) {
				if (scrcpy_should_stop(session))
					goto cleanup_winsock;

				if (!scrcpy_open_video_socket(session))
					goto cleanup_winsock;

				/*
			 * scrcpy server accepts all sockets (video, then audio, then control)
			 * BEFORE sending the dummy byte and handshake on the video socket.
			 * We must connect the audio socket here, otherwise the server will
			 * block in accept() and our read_handshake will deadlock.
			 */
				if (session->audio_enabled && session->on_audio) {
					if (!scrcpy_open_audio_socket(session)) {
						if (session->video_socket != INVALID_SOCKET) {
							shutdown(session->video_socket, SD_BOTH);
							closesocket(session->video_socket);
							session->video_socket = INVALID_SOCKET;
						}
						Sleep(500);
						continue;
					}
				}

				if (scrcpy_read_handshake(session, &codec_id, &width, &height)) {
					handshake_ok = true;
					reconnect_attempts = 0;
					break;
				}

				/* Handshake failed - server probably not ready. Close sockets and retry. */
				obs_log(LOG_DEBUG, "scrcpy handshake attempt %d/%d failed, retrying in 500ms",
					connect_attempt + 1, max_attempts);
				if (session->video_socket != INVALID_SOCKET) {
					shutdown(session->video_socket, SD_BOTH);
					closesocket(session->video_socket);
					session->video_socket = INVALID_SOCKET;
				}
				if (session->audio_socket != INVALID_SOCKET) {
					shutdown(session->audio_socket, SD_BOTH);
					closesocket(session->audio_socket);
					session->audio_socket = INVALID_SOCKET;
				}
				Sleep(500);
			}
			if (!handshake_ok) {
				obs_log(LOG_ERROR, "scrcpy handshake failed after %d attempts", connect_attempt);
				goto cleanup_winsock;
			}
		}

		if (!scrcpy_init_decoder(session, codec_id, &decoder_context))
			goto cleanup_winsock;

		/*
	 * Audio socket is already connected. Start the audio worker on a separate thread
	 * so video and audio decode loops run concurrently.
	 */
		if (session->audio_enabled && session->on_audio) {
			if (session->audio_socket != INVALID_SOCKET) {
				uintptr_t audio_handle = _beginthreadex(NULL, 0, scrcpy_audio_worker, session, 0, NULL);
				if (audio_handle) {
					session->audio_thread = (HANDLE)audio_handle;
					obs_log(LOG_INFO, "scrcpy audio worker thread started");
				} else {
					obs_log(LOG_WARNING, "failed to start audio worker thread");
				}
			}
		}

		if (!scrcpy_decode_loop(session, decoder_context, width, height))
			obs_log(LOG_WARNING, "scrcpy decode loop exited due to stream error or decode failure");

		obs_log(LOG_INFO, "scrcpy bootstrap finished for device '%s' on tcp:%hu", session->device_serial,
			session->local_port);

	cleanup_winsock:
		if (decoder_context)
			avcodec_free_context(&decoder_context);

		/* Wait for audio thread to finish before closing handles */
		if (session->audio_thread) {
			WaitForSingleObject(session->audio_thread, 5000);
			CloseHandle(session->audio_thread);
			session->audio_thread = NULL;
		}

		scrcpy_close_stream_handles(session);

		_snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" -s %s forward --remove tcp:%hu",
			    session->adb_path, session->device_serial, session->local_port);
		if (!scrcpy_run_process(session, "Remove adb forward", command, true, true, NULL)) {
			obs_log(LOG_INFO, "adb forward removal is optional during cleanup for tcp:%hu",
				session->local_port);
		}

		if (scrcpy_should_stop(session))
			break;

		reconnect_attempts++;
		if (reconnect_attempts >= 100) {
			obs_log(LOG_ERROR, "scrcpy auto-reconnect failed 100 times. Giving up.");
			break;
		}

		int delay_ms = 2000;
		if (reconnect_attempts >= 50) {
			delay_ms = 10000;
		} else if (reconnect_attempts >= 15) {
			delay_ms = 5000;
		}

		obs_log(LOG_INFO, "scrcpy auto-reconnect attempt %d, waiting %d ms...", reconnect_attempts, delay_ms);

		for (int i = 0; i < delay_ms; i += 200) {
			if (scrcpy_should_stop(session))
				break;
			Sleep(200);
		}
	}

	WSACleanup();

	InterlockedExchange(&session->running, 0);
	return 0;
}
