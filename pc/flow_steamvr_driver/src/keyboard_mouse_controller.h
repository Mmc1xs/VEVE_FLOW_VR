//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <thread>

#include "openvr_driver.h"

enum FlowKeyboardMouseComponent
{
	FlowKeyboardMouseComponent_trigger_value,
	FlowKeyboardMouseComponent_trigger_click,
	FlowKeyboardMouseComponent_grip_click,
	FlowKeyboardMouseComponent_trackpad_x,
	FlowKeyboardMouseComponent_trackpad_y,
	FlowKeyboardMouseComponent_trackpad_touch,
	FlowKeyboardMouseComponent_trackpad_click,
	FlowKeyboardMouseComponent_application_menu_click,
	FlowKeyboardMouseComponent_system_click,
	FlowKeyboardMouseComponent_haptic,
	FlowKeyboardMouseComponent_pose_raw,
	FlowKeyboardMouseComponent_pose_tip,

	FlowKeyboardMouseComponent_MAX
};

class FlowKeyboardMouseControllerDevice : public vr::ITrackedDeviceServerDriver
{
public:
	FlowKeyboardMouseControllerDevice();

	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;
	void Deactivate() override;

	const std::string &MyGetSerialNumber() const;
	void MyRunFrame();
	void MyProcessEvent( const vr::VREvent_t &vrevent );

private:
	void PoseUpdateThread();
	void KeypadReceiveThread();

	std::atomic< vr::TrackedDeviceIndex_t > device_index_{ vr::k_unTrackedDeviceIndexInvalid };
	std::atomic< bool > is_active_{ false };
	std::thread pose_update_thread_;
	std::thread keypad_receive_thread_;
	// Pressed-button mask from the dashboard helper and when it last arrived (steady ms).
	std::atomic< uint32_t > keypad_buttons_{ 0 };
	std::atomic< int64_t > keypad_updated_ms_{ 0 };

	std::string model_number_{ "Flow Keypad Controller" };
	std::string serial_number_{ "FLOW-KBM-RIGHT-001" };

	std::array< vr::VRInputComponentHandle_t, FlowKeyboardMouseComponent_MAX > input_handles_{};
};
