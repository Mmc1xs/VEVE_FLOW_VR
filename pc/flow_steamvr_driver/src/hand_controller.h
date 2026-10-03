#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "flow_pointer_gate.h"
#include "flow_shared_input.h"
#include "openvr_driver.h"

enum FlowHandComponent
{
	FlowHandComponent_system_click,
	FlowHandComponent_system_touch,
	FlowHandComponent_a_click,
	FlowHandComponent_a_touch,
	FlowHandComponent_b_click,
	FlowHandComponent_b_touch,
	FlowHandComponent_trigger_click,
	FlowHandComponent_trigger_touch,
	FlowHandComponent_trigger_value,
	FlowHandComponent_grip_touch,
	FlowHandComponent_grip_force,
	FlowHandComponent_grip_value,
	FlowHandComponent_thumbstick_click,
	FlowHandComponent_thumbstick_touch,
	FlowHandComponent_thumbstick_x,
	FlowHandComponent_thumbstick_y,
	FlowHandComponent_finger_index,
	FlowHandComponent_finger_middle,
	FlowHandComponent_finger_ring,
	FlowHandComponent_finger_pinky,
	FlowHandComponent_haptic,

	FlowHandComponent_MAX
};

// A Valve Index ("knuckles") controller driven by one of the Flow's tracked hands: pose from the
// hand joints, trigger from the index pinch, grip from the curl of the other fingers. The right
// hand also takes the numeric keypad (thumbstick, A/B, system) while it is tracked; otherwise the
// head-aimed keypad controller handles the keypad.
class FlowHandControllerDevice : public vr::ITrackedDeviceServerDriver
{
public:
	explicit FlowHandControllerDevice( FlowHandSide side );

	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;
	void Deactivate() override;

	const std::string &MyGetSerialNumber() const;
	void MyRunFrame();

private:
	void PoseUpdateThread();
	// Connected once the hand is tracked, until it has been lost for a while (no flapping when
	// tracking drops for a moment).
	void UpdateConnected( bool tracked );

	FlowHandSide side_;
	std::string serial_number_;
	std::atomic< vr::TrackedDeviceIndex_t > device_index_{ vr::k_unTrackedDeviceIndexInvalid };
	std::atomic< bool > is_active_{ false };
	std::atomic< bool > connected_{ false };
	std::atomic< int64_t > last_tracked_ms_{ 0 };
	bool trigger_clicked_ = false;
	int64_t last_input_log_ms_ = 0;
	float pitch_offset_rad_ = 0.0f;
	std::thread pose_update_thread_;
	std::array< vr::VRInputComponentHandle_t, FlowHandComponent_MAX > input_handles_{};
	FlowPointerGate pointer_gate_;
};
