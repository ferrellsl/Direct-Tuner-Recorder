/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "Device.h"
#include "Guide.h"
#include "Json.h"
#include "RecordingStore.h"
#include "Scheduler.h"

namespace dt
{

// JSON for the core's types. The state files and the recorder service's API share these, so
// both always agree on the fields.

Json EventToJson(const GuideEvent& event);
GuideEvent EventFromJson(const Json& json);

// withState adds the live state, for the API. State files leave it out: a timer that was
// recording when the program stopped must start again as scheduled.
Json TimerToJson(const Timer& timer, bool withState = false);
Timer TimerFromJson(const Json& json);

// withId adds the id and file size, for the API. Sidecar files leave them out because the id is
// the file's own path.
Json RecordingToJson(const RecordingInfo& info, bool withId = false);
RecordingInfo RecordingFromJson(const Json& json);

Json ChannelToJson(const Channel& channel);
Channel ChannelFromJson(const Json& json);

} // namespace dt
