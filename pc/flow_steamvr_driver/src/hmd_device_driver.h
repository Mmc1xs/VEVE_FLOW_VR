//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include <array>
#include <string>

#include "openvr_driver.h"
#include "virtual_display_device.h"
#include <atomic>
#include <mutex>
#include <thread>

enum MyComponent
{
	MyComponent_head_proximity_click,

	MyComponent_MAX
};

struct MyHMDDisplayDriverConfiguration
{
	int32_t window_x;
	int32_t window_y;

	int32_t window_width;
	int32_t window_height;

	int32_t render_width;
	int32_t render_height;

	// Lens frustum as tangents in Wave convention (left/bottom negative), from
	// WVR_GetClippingPlaneBoundary on the Flow. Same for both eyes on Flow.
	float tan_left;
	float tan_right;
	float tan_top;
	float tan_bottom;
};

class MyHMDDisplayComponent : public vr::IVRDisplayComponent
{
public:
	explicit MyHMDDisplayComponent( const MyHMDDisplayDriverConfiguration &config );

	// ----- Functions to override vr::IVRDisplayComponent -----
	bool IsDisplayOnDesktop() override;
	bool IsDisplayRealDisplay() override;
	void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) override;
	void GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) override;
	void GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) override;
	vr::DistortionCoordinates_t ComputeDistortion( vr::EVREye eEye, float fU, float fV ) override;
	void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) override;
	bool ComputeInverseDistortion( vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override;

private:
	MyHMDDisplayDriverConfiguration config_;
};

//-----------------------------------------------------------------------------
// Purpose: Represents a single tracked device in the system.
// What this device actually is (controller, hmd) depends on what the
// IServerTrackedDeviceProvider calls to TrackedDeviceAdded and the
// properties within Activate() of the ITrackedDeviceServerDriver class.
//-----------------------------------------------------------------------------
class MyHMDControllerDeviceDriver : public vr::ITrackedDeviceServerDriver
{
public:
	MyHMDControllerDeviceDriver();
	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;
	void Deactivate() override;

	// ----- Functions we declare ourselves below -----
	const std::string &MyGetSerialNumber();
	void MyRunFrame();
	void MyProcessEvent( const vr::VREvent_t &vrevent );
	void MyPoseUpdateThread();
	void MyPoseReceiveThread();

private:
	struct FlowPose
	{
		float x = 0.0f;
		float y = 1.0f;
		float z = 0.0f;
		float qx = 0.0f;
		float qy = 0.0f;
		float qz = 0.0f;
		float qw = 1.0f;
		uint32_t sequence = 0;
		std::chrono::steady_clock::time_point received_at = std::chrono::steady_clock::time_point::min();
	};

	// True while the Flow keeps sending poses (it stops when taken off and put to sleep).
	bool IsFlowPoseFresh( const FlowPose &pose, std::chrono::steady_clock::time_point now ) const;

	std::unique_ptr< MyHMDDisplayComponent > my_display_component_;
	std::unique_ptr< FlowVirtualDisplayDevice > my_virtual_display_component_;

	std::string my_hmd_model_number_;
	std::string my_hmd_serial_number_;

	std::array< vr::VRInputComponentHandle_t, MyComponent_MAX > my_input_handles_{};
	std::atomic< int > frame_number_;
	std::atomic< bool > is_active_;
	std::atomic< uint32_t > device_index_;

	std::thread my_pose_update_thread_;
	std::thread my_pose_receive_thread_;
	std::mutex pose_mutex_;
	FlowPose latest_pose_;
	bool user_present_ = false; // last value sent on /proximity
};
