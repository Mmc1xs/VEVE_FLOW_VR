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

// Tracking universe of the Flow HMD and the keypad controller ("FLOW"). SteamVR keeps chaperone
// data (seated/standing zero pose, play area) per universe; without one the seated zero pose is
// never calibrated, seated apps (most Unity titles) get invalid poses and the compositor fades
// them to trackingLossColor. pc/flow_dashboard_helper fills in the chaperone data if missing.
constexpr uint64_t kFlowTrackingUniverseId = 0x464C4F57;

// Added to the Flow's head-origin heights (head and hands) so the head sits at standing height.
constexpr float kFlowStandingHeightOffset = 1.0f;
