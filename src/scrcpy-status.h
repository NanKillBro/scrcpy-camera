/*
 * scrcpy session status model
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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Everything the UI needs to tell the user what the session is doing, or why it
 * stopped doing it. The session worker fills this in as it walks the bootstrap
 * sequence; the source owns the storage and renders it into the properties panel.
 *
 * Deliberately free of any obs/ffmpeg types so both sides can include it.
 */

#define SCRCPY_STATUS_DETAIL_SIZE 512
/* The scrcpy handshake sends a fixed 64-byte device name, which may use all 64
 * bytes, so this leaves room for the terminator. */
#define SCRCPY_STATUS_NAME_SIZE 65
/* Rendered banner: headline + advice + device name + escaped detail + markup. */
#define SCRCPY_STATUS_TEXT_SIZE 2048

/* Mirrors the order of steps in scrcpy_session_worker(). */
enum scrcpy_status_phase {
	SCRCPY_PHASE_IDLE,          /* source not showing and not active */
	SCRCPY_PHASE_NO_DEVICE,     /* no serial picked in the dropdown */
	SCRCPY_PHASE_ADB_SERVER,    /* adb start-server */
	SCRCPY_PHASE_PUSH_SERVER,   /* adb push scrcpy-server.jar */
	SCRCPY_PHASE_FORWARD,       /* adb forward tcp:<port> */
	SCRCPY_PHASE_LAUNCH_SERVER, /* app_process com.genymobile.scrcpy.Server */
	SCRCPY_PHASE_HANDSHAKE,     /* connect + read device metadata */
	SCRCPY_PHASE_DECODER_INIT,  /* avcodec open */
	SCRCPY_PHASE_STREAMING,     /* frames flowing */
	SCRCPY_PHASE_RECONNECTING,  /* stream died, backing off before retry */
	SCRCPY_PHASE_FAILED,        /* terminal; gave up retrying */
};

/*
 * Set when a phase fails. Drives both the headline and the "what to do about it"
 * line, so each value must correspond to one actionable user-facing cause.
 */
enum scrcpy_status_problem {
	SCRCPY_PROBLEM_NONE,
	SCRCPY_PROBLEM_ADB_NOT_FOUND,
	SCRCPY_PROBLEM_NO_DEVICES,
	SCRCPY_PROBLEM_DEVICE_NOT_FOUND,
	SCRCPY_PROBLEM_DEVICE_UNAUTHORIZED,
	SCRCPY_PROBLEM_DEVICE_AUTHORIZING,
	SCRCPY_PROBLEM_DEVICE_OFFLINE,
	SCRCPY_PROBLEM_JAR_MISSING,
	SCRCPY_PROBLEM_PORT_IN_USE,
	SCRCPY_PROBLEM_VERSION_MISMATCH,
	SCRCPY_PROBLEM_CAMERA_ID_INVALID,
	SCRCPY_PROBLEM_CAMERA_CONFIG,
	SCRCPY_PROBLEM_CAMERA_UNSUPPORTED,
	SCRCPY_PROBLEM_AUDIO_UNSUPPORTED,
	SCRCPY_PROBLEM_AUDIO_CODEC,
	SCRCPY_PROBLEM_DECODER_MISSING,
	SCRCPY_PROBLEM_SERVER_SILENT,
	SCRCPY_PROBLEM_SERVER_CRASH,
	SCRCPY_PROBLEM_UNKNOWN,
};

enum scrcpy_status_severity {
	SCRCPY_SEVERITY_INFO,
	SCRCPY_SEVERITY_WARNING,
	SCRCPY_SEVERITY_ERROR,
};

struct scrcpy_status {
	enum scrcpy_status_phase phase;
	enum scrcpy_status_problem problem;

	/* Verbatim tail of whatever adb or the scrcpy server printed. Shown as-is
	 * so advanced users get the raw text without opening the OBS log. */
	char detail[SCRCPY_STATUS_DETAIL_SIZE];
	/* Device model reported in the 64-byte handshake metadata. */
	char device_name[SCRCPY_STATUS_NAME_SIZE];

	uint32_t width;
	uint32_t height;
	char codec[16];
	char hw_name[32];
	bool hw_decode;
	bool audio_active;
	/*
	 * Audio is optional and its failures do not stop video, so it gets its own
	 * slot rather than overwriting `problem` and making a working stream look
	 * broken.
	 */
	enum scrcpy_status_problem audio_problem;

	/* Handshake progress, so a slow camera start looks like progress rather
	 * than a hang. */
	int attempt;
	int max_attempts;
	int reconnect_count;
};

typedef void (*scrcpy_session_status_callback)(void *opaque, const struct scrcpy_status *status);

/* Human-readable pieces used to build the properties banner. */
const char *scrcpy_status_phase_label(enum scrcpy_status_phase phase);
const char *scrcpy_status_problem_title(enum scrcpy_status_problem problem);
const char *scrcpy_status_problem_advice(enum scrcpy_status_problem problem);
enum scrcpy_status_severity scrcpy_status_severity(const struct scrcpy_status *status);

/*
 * Renders the banner into dest as Qt rich text. OBS draws OBS_TEXT_INFO through a
 * QLabel, so <b>, <br> and inline styles all work.
 */
void scrcpy_status_format(const struct scrcpy_status *status, char *dest, size_t dest_size);
