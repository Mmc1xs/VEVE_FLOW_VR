//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "device_provider.h"

#include "driverlog.h"

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver after it receives a pointer back from HmdDriverFactory.
// You should do your resources allocations here (**not** in the constructor).
//-----------------------------------------------------------------------------
vr::EVRInitError MyDeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
	// We need to initialise our driver context to make calls to the server.
	// OpenVR provides a macro to do this for us.
	VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

	// First, initialize our hmd, which we'll later pass OpenVR a pointer to.
	my_hmd_device_ = std::make_unique< MyHMDControllerDeviceDriver >();
	my_virtual_display_device_ = std::make_unique< FlowVirtualDisplayDevice >();
	// Numeric-keypad controller (buttons from the keypad via flow_dashboard_helper, aimed by the head).
	if ( vr::VRSettings()->GetBool( "driver_flowvr", "enable_keypad_controller" ) )
	{
		my_keyboard_mouse_controller_ = std::make_unique< FlowKeyboardMouseControllerDevice >();
	}
	DriverLog( "Flow keypad controller %s", my_keyboard_mouse_controller_ ? "enabled" : "disabled" );
	// Index controllers driven by the Flow's hand tracking.
	const bool hands_enabled = vr::VRSettings()->GetBool( "driver_flowvr", "enable_hand_controllers" );
	if ( hands_enabled )
	{
		my_hand_controllers_[ FlowHand_Left ] = std::make_unique< FlowHandControllerDevice >( FlowHand_Left );
		my_hand_controllers_[ FlowHand_Right ] = std::make_unique< FlowHandControllerDevice >( FlowHand_Right );
	}
	DriverLog( "Flow hand controllers %s", hands_enabled ? "enabled" : "disabled" );

	// TrackedDeviceAdded returning true means we have had our device added to SteamVR.
	if ( !vr::VRServerDriverHost()->TrackedDeviceAdded( my_hmd_device_->MyGetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, my_hmd_device_.get() ) )
	{
		DriverLog( "Failed to create hmd device!" );
		return vr::VRInitError_Driver_Unknown;
	}

	if ( !vr::VRServerDriverHost()->TrackedDeviceAdded( my_virtual_display_device_->MyGetSerialNumber().c_str(), vr::TrackedDeviceClass_DisplayRedirect, my_virtual_display_device_.get() ) )
	{
		DriverLog( "Failed to create virtual display redirect device!" );
		return vr::VRInitError_Driver_Unknown;
	}

	if ( my_keyboard_mouse_controller_ != nullptr &&
	     !vr::VRServerDriverHost()->TrackedDeviceAdded( my_keyboard_mouse_controller_->MyGetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, my_keyboard_mouse_controller_.get() ) )
	{
		DriverLog( "Failed to create keyboard/mouse controller device!" );
		return vr::VRInitError_Driver_Unknown;
	}

	for ( const auto &hand : my_hand_controllers_ )
	{
		if ( hand != nullptr &&
		     !vr::VRServerDriverHost()->TrackedDeviceAdded( hand->MyGetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, hand.get() ) )
		{
			DriverLog( "Failed to create hand controller device %s!", hand->MyGetSerialNumber().c_str() );
			return vr::VRInitError_Driver_Unknown;
		}
	}

	return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: Tells the runtime which version of the API we are targeting.
// Helper variables in the header you're using contain this information, which can be returned here.
//-----------------------------------------------------------------------------
const char *const *MyDeviceProvider::GetInterfaceVersions()
{
	return vr::k_InterfaceVersions;
}

//-----------------------------------------------------------------------------
// Purpose: This function is deprecated and never called. But, it must still be defined, or we can't compile.
//-----------------------------------------------------------------------------
bool MyDeviceProvider::ShouldBlockStandbyMode()
{
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: This is called in the main loop of vrserver.
// Drivers *can* do work here, but should ensure this work is relatively inexpensive.
// A good thing to do here is poll for events from the runtime or applications
//-----------------------------------------------------------------------------
void MyDeviceProvider::RunFrame()
{
	// call our devices to run a frame
	if ( my_hmd_device_ != nullptr )
	{
		my_hmd_device_->MyRunFrame();
	}

	if ( my_keyboard_mouse_controller_ != nullptr )
	{
		my_keyboard_mouse_controller_->MyRunFrame();
	}
	for ( const auto &hand : my_hand_controllers_ )
	{
		if ( hand != nullptr )
		{
			hand->MyRunFrame();
		}
	}


	//Now, process events that were submitted for this frame.
	vr::VREvent_t vrevent{};
	while ( vr::VRServerDriverHost()->PollNextEvent( &vrevent, sizeof( vr::VREvent_t ) ) )
	{
		if ( my_hmd_device_ != nullptr )
		{
			my_hmd_device_->MyProcessEvent( vrevent );
		}
		if ( my_keyboard_mouse_controller_ != nullptr )
		{
			my_keyboard_mouse_controller_->MyProcessEvent( vrevent );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: This function is called when the system enters a period of inactivity.
// The devices might want to turn off their displays or go into a low power mode to preserve them.
//-----------------------------------------------------------------------------
void MyDeviceProvider::EnterStandby()
{
}

//-----------------------------------------------------------------------------
// Purpose: This function is called after the system has been in a period of inactivity, and is waking up again.
// Turn back on the displays or devices here.
//-----------------------------------------------------------------------------
void MyDeviceProvider::LeaveStandby()
{
}

//-----------------------------------------------------------------------------
// Purpose: This function is called just before the driver is unloaded from vrserver.
// Drivers should free whatever resources they have acquired over the session here.
// Any calls to the server is guaranteed to be valid before this, but not after it has been called.
//-----------------------------------------------------------------------------
void MyDeviceProvider::Cleanup()
{
	// Our controller devices will have already deactivated. Let's now destroy them.
	my_keyboard_mouse_controller_ = nullptr;
	my_hand_controllers_[ FlowHand_Left ] = nullptr;
	my_hand_controllers_[ FlowHand_Right ] = nullptr;
	my_virtual_display_device_ = nullptr;
	my_hmd_device_ = nullptr;
}
