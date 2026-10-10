/*
 * scrcpy session status text
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "scrcpy-status.h"

/*
 * Wording rules for everything below: name the state, then the cause, then the
 * one thing the user should do next. No error codes in the headline; the raw
 * adb/server text is appended separately for people who want it.
 */

const char *scrcpy_status_phase_label(enum scrcpy_status_phase phase)
{
	switch (phase) {
	case SCRCPY_PHASE_IDLE:
		return "Not running";
	case SCRCPY_PHASE_NO_DEVICE:
		return "No device selected";
	case SCRCPY_PHASE_ADB_SERVER:
		return "Starting ADB server";
	case SCRCPY_PHASE_PUSH_SERVER:
		return "Sending scrcpy-server.jar to the device";
	case SCRCPY_PHASE_FORWARD:
		return "Setting up the ADB port forward";
	case SCRCPY_PHASE_LAUNCH_SERVER:
		return "Starting scrcpy on the device";
	case SCRCPY_PHASE_HANDSHAKE:
		return "Waiting for the device to start streaming";
	case SCRCPY_PHASE_DECODER_INIT:
		return "Opening the video decoder";
	case SCRCPY_PHASE_STREAMING:
		return "Streaming";
	case SCRCPY_PHASE_RECONNECTING:
		return "Reconnecting";
	case SCRCPY_PHASE_FAILED:
		return "Stopped";
	}
	return "Unknown";
}

const char *scrcpy_status_problem_title(enum scrcpy_status_problem problem)
{
	switch (problem) {
	case SCRCPY_PROBLEM_NONE:
		return "";
	case SCRCPY_PROBLEM_ADB_NOT_FOUND:
		return "ADB executable not found";
	case SCRCPY_PROBLEM_NO_DEVICES:
		return "No Android device connected";
	case SCRCPY_PROBLEM_DEVICE_NOT_FOUND:
		return "Device disconnected";
	case SCRCPY_PROBLEM_DEVICE_UNAUTHORIZED:
		return "Device not authorized";
	case SCRCPY_PROBLEM_DEVICE_AUTHORIZING:
		return "Device is still authorizing";
	case SCRCPY_PROBLEM_DEVICE_OFFLINE:
		return "Device is offline";
	case SCRCPY_PROBLEM_JAR_MISSING:
		return "scrcpy-server.jar not found";
	case SCRCPY_PROBLEM_PORT_IN_USE:
		return "Local TCP port already in use";
	case SCRCPY_PROBLEM_VERSION_MISMATCH:
		return "Wrong scrcpy-server.jar version";
	case SCRCPY_PROBLEM_CAMERA_ID_INVALID:
		return "Camera ID does not exist on this device";
	case SCRCPY_PROBLEM_CAMERA_CONFIG:
		return "The device rejected this camera size";
	case SCRCPY_PROBLEM_CAMERA_UNSUPPORTED:
		return "Camera capture needs a newer Android version";
	case SCRCPY_PROBLEM_AUDIO_UNSUPPORTED:
		return "Audio capture is not supported by this device";
	case SCRCPY_PROBLEM_AUDIO_CODEC:
		return "Audio codec not supported";
	case SCRCPY_PROBLEM_DECODER_MISSING:
		return "No decoder for this video codec";
	case SCRCPY_PROBLEM_SERVER_SILENT:
		return "The device never started streaming";
	case SCRCPY_PROBLEM_SERVER_CRASH:
		return "scrcpy stopped on the device";
	case SCRCPY_PROBLEM_UNKNOWN:
		return "Could not start the stream";
	}
	return "Could not start the stream";
}

const char *scrcpy_status_problem_advice(enum scrcpy_status_problem problem)
{
	switch (problem) {
	case SCRCPY_PROBLEM_NONE:
		return "";
	case SCRCPY_PROBLEM_ADB_NOT_FOUND:
		return "Check the \"ADB executable\" path above. Install Android platform-tools "
		       "if you do not have adb yet.";
	case SCRCPY_PROBLEM_NO_DEVICES:
		return "Connect your device by USB, enable <b>USB debugging</b> in Developer "
		       "options, then click \"Refresh device list\".";
	case SCRCPY_PROBLEM_DEVICE_NOT_FOUND:
		return "The device is no longer reachable. Reconnect the cable, or re-pair it if "
		       "you are using wireless debugging.";
	case SCRCPY_PROBLEM_DEVICE_UNAUTHORIZED:
		return "Look at your device screen and tap <b>Allow</b> on the \"Allow USB "
		       "debugging?\" prompt. Tick \"Always allow\" to stop it asking again.";
	case SCRCPY_PROBLEM_DEVICE_AUTHORIZING:
		return "The device is still accepting the debugging key. This should clear on its own "
		       "in a moment.";
	case SCRCPY_PROBLEM_DEVICE_OFFLINE:
		return "Unplug and replug the device. If it stays offline, revoke USB debugging "
		       "authorizations in Developer options and reconnect.";
	case SCRCPY_PROBLEM_JAR_MISSING:
		return "Check the \"scrcpy-server.jar path\" above and make sure the file exists.";
	case SCRCPY_PROBLEM_PORT_IN_USE:
		return "Another program (often a second scrcpy source, or scrcpy itself) is holding "
		       "this port. Change \"Local TCP port\" to a free value.";
	case SCRCPY_PROBLEM_VERSION_MISMATCH:
		return "Download <b>scrcpy-server-v4.0</b> from the scrcpy v4.0 release, rename it to "
		       "scrcpy-server.jar, and point \"scrcpy-server.jar path\" at it. Only v4.0 works.";
	case SCRCPY_PROBLEM_CAMERA_ID_INVALID:
		return "Pick one of the camera IDs listed below in \"Camera ID\".";
	case SCRCPY_PROBLEM_CAMERA_CONFIG:
		return "Try a \"Camera Size\" the camera actually supports (1920x1080 is a safe "
		       "choice), or clear it and use \"Max resolution\" instead.";
	case SCRCPY_PROBLEM_CAMERA_UNSUPPORTED:
		return "Camera capture needs Android 12 or newer. Switch \"Video source\" to Screen.";
	case SCRCPY_PROBLEM_AUDIO_UNSUPPORTED:
		return "Audio forwarding needs Android 11 or newer (Playback needs Android 13). "
		       "Turn off \"Enable Audio\" to stream video only.";
	case SCRCPY_PROBLEM_AUDIO_CODEC:
		return "Your device or this FFmpeg build cannot handle the selected audio codec. "
		       "Try <b>AAC</b> instead of Opus.";
	case SCRCPY_PROBLEM_DECODER_MISSING:
		return "This OBS build's FFmpeg has no decoder for the selected codec. Switch "
		       "\"Video codec\" to H.264 (AVC).";
	case SCRCPY_PROBLEM_SERVER_SILENT:
		return "scrcpy was launched but never sent any video. Check that the "
		       "scrcpy-server.jar version is v4.0, and that the screen is unlocked.";
	case SCRCPY_PROBLEM_SERVER_CRASH:
		return "scrcpy exited on the device. The message below is what it reported.";
	case SCRCPY_PROBLEM_UNKNOWN:
		return "See the message below, or the OBS log, for details.";
	}
	return "";
}

enum scrcpy_status_severity scrcpy_status_severity(const struct scrcpy_status *status)
{
	if (!status)
		return SCRCPY_SEVERITY_INFO;

	if (status->problem != SCRCPY_PROBLEM_NONE) {
		/*
		 * Anything we are still actively retrying is a warning, not an error:
		 * the plugin keeps working and these clear up on their own once the
		 * user plugs the cable back in or taps Allow.
		 */
		if (status->phase == SCRCPY_PHASE_RECONNECTING || status->problem == SCRCPY_PROBLEM_DEVICE_AUTHORIZING)
			return SCRCPY_SEVERITY_WARNING;
		return SCRCPY_SEVERITY_ERROR;
	}

	if (status->phase == SCRCPY_PHASE_NO_DEVICE)
		return SCRCPY_SEVERITY_WARNING;

	return SCRCPY_SEVERITY_INFO;
}

/* Appends to dest, never overflowing it, and keeps *len in sync. */
static void status_append(char *dest, size_t dest_size, size_t *len, const char *text)
{
	if (!text || !*text || *len + 1 >= dest_size)
		return;

	size_t remaining = dest_size - *len - 1;
	size_t text_len = strlen(text);

	if (text_len > remaining)
		text_len = remaining;

	memcpy(dest + *len, text, text_len);
	*len += text_len;
	dest[*len] = '\0';
}

static void status_appendf(char *dest, size_t dest_size, size_t *len, const char *format, ...)
{
	char buffer[SCRCPY_STATUS_DETAIL_SIZE + 128];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	status_append(dest, dest_size, len, buffer);
}

/*
 * Escapes the characters that would otherwise be read as markup. The detail text
 * comes straight from adb/scrcpy output, so it must never be trusted as rich text.
 */
static void status_append_escaped(char *dest, size_t dest_size, size_t *len, const char *text)
{
	for (const char *p = text; p && *p; ++p) {
		switch (*p) {
		case '<':
			status_append(dest, dest_size, len, "&lt;");
			break;
		case '>':
			status_append(dest, dest_size, len, "&gt;");
			break;
		case '&':
			status_append(dest, dest_size, len, "&amp;");
			break;
		case '\n':
			status_append(dest, dest_size, len, "<br>");
			break;
		case '\r':
			break;
		default: {
			char one[2] = {*p, '\0'};
			status_append(dest, dest_size, len, one);
			break;
		}
		}
	}
}

void scrcpy_status_format(const struct scrcpy_status *status, char *dest, size_t dest_size)
{
	size_t len = 0;

	if (!dest || dest_size == 0)
		return;

	dest[0] = '\0';

	if (!status) {
		status_append(dest, dest_size, &len, "Status unavailable");
		return;
	}

	enum scrcpy_status_severity severity = scrcpy_status_severity(status);
	bool has_problem = status->problem != SCRCPY_PROBLEM_NONE;

	/* Headline. The leading glyph survives themes that do not style the label. */
	const char *glyph = "";
	switch (severity) {
	case SCRCPY_SEVERITY_INFO:
		glyph = status->phase == SCRCPY_PHASE_STREAMING ? "&#9679; " : "&#8230; ";
		break;
	case SCRCPY_SEVERITY_WARNING:
		glyph = "&#9888; ";
		break;
	case SCRCPY_SEVERITY_ERROR:
		glyph = "&#10006; ";
		break;
	}

	status_append(dest, dest_size, &len, "<b>");
	status_append(dest, dest_size, &len, glyph);
	if (has_problem)
		status_append(dest, dest_size, &len, scrcpy_status_problem_title(status->problem));
	else
		status_append(dest, dest_size, &len, scrcpy_status_phase_label(status->phase));
	status_append(dest, dest_size, &len, "</b>");

	/* While retrying, say what is being retried so the headline is not ambiguous. */
	if (status->phase == SCRCPY_PHASE_RECONNECTING && status->reconnect_count > 0)
		status_appendf(dest, dest_size, &len, " &mdash; retrying (attempt %d)", status->reconnect_count);
	else if (status->phase == SCRCPY_PHASE_HANDSHAKE && status->max_attempts > 0)
		status_appendf(dest, dest_size, &len, " &mdash; attempt %d of %d", status->attempt,
			       status->max_attempts);

	/* Healthy stream: the facts a streamer actually wants to confirm. */
	if (status->phase == SCRCPY_PHASE_STREAMING && !has_problem) {
		status_append(dest, dest_size, &len, "<br>");
		if (status->width && status->height)
			status_appendf(dest, dest_size, &len, "%ux%u", status->width, status->height);
		if (status->codec[0])
			status_appendf(dest, dest_size, &len, " &middot; %s", status->codec);
		status_appendf(dest, dest_size, &len, " &middot; %s",
			       status->hw_decode && status->hw_name[0] ? status->hw_name
								       : (status->hw_decode ? "hardware" : "software"));
		status_appendf(dest, dest_size, &len, " decoding &middot; audio %s",
			       status->audio_active ? "on" : "off");
	}

	const char *advice = scrcpy_status_problem_advice(status->problem);
	if (has_problem && advice[0]) {
		status_append(dest, dest_size, &len, "<br>");
		status_append(dest, dest_size, &len, advice);
	}

	/*
	 * Audio trouble on an otherwise healthy stream gets its own line, so the
	 * user is not left wondering why the meter is silent.
	 */
	if (status->audio_problem != SCRCPY_PROBLEM_NONE) {
		status_append(dest, dest_size, &len, "<br><b>Audio: ");
		status_append(dest, dest_size, &len, scrcpy_status_problem_title(status->audio_problem));
		status_append(dest, dest_size, &len, "</b> ");
		status_append(dest, dest_size, &len, scrcpy_status_problem_advice(status->audio_problem));
	}

	if (status->device_name[0]) {
		status_append(dest, dest_size, &len, "<br><i>Device: ");
		status_append_escaped(dest, dest_size, &len, status->device_name);
		status_append(dest, dest_size, &len, "</i>");
	}

	if (status->detail[0]) {
		status_append(dest, dest_size, &len, "<br><br>");
		status_append_escaped(dest, dest_size, &len, status->detail);
	}
}
