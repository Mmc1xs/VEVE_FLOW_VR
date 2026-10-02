//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include <atomic>
#include <cstdint>

// Sequence number of the Flow pose most recently handed to SteamVR (0 = no fresh pose).
// Written by the HMD pose path and stamped onto each streamed frame, so the Flow can
// tell its timewarp which head pose the frame was rendered with.
inline std::atomic< uint32_t > g_flow_pose_sequence_in_use{ 0 };

// Bool on the HMD device: true while the Flow is connected to the video stream.
// Read by pc/flow_dashboard_helper (keep the id in sync there) to open the dashboard
// only once the headset is actually showing our picture.
constexpr int kProp_FlowStreamConnected_Bool = 10001; // vr::Prop_VendorSpecific_Reserved_Start + 1
