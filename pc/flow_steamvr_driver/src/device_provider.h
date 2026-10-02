//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include <memory>

#include "hmd_device_driver.h"
#include "keyboard_mouse_controller.h"
#include "openvr_driver.h"
#include "virtual_display_device.h"

// make sure your class is publicly inheriting vr::IServerTrackedDeviceProvider!
class MyDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init( vr::IVRDriverContext *pDriverContext ) override;
	const char *const *GetInterfaceVersions() override;

	void RunFrame() override;

	bool ShouldBlockStandbyMode() override;
	void EnterStandby() override;
	void LeaveStandby() override;

	void Cleanup() override;

private:
	std::unique_ptr<MyHMDControllerDeviceDriver> my_hmd_device_;
	std::unique_ptr<FlowVirtualDisplayDevice> my_virtual_display_device_;
	std::unique_ptr<FlowKeyboardMouseControllerDevice> my_keyboard_mouse_controller_;
};
