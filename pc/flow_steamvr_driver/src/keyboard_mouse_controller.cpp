//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "keyboard_mouse_controller.h"

#include "driverlog.h"
#include "flow_pointer_gate.h"
#include "flow_pose_sync.h"
#include "flow_shared_input.h"
#include "vrmath.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// Keypad controller: a right-hand controller whose buttons come from the PC's numeric keypad
// and whose laser follows the head, so the user aims with the head and clicks on the keypad.
// pc/flow_dashboard_helper captures the keypad (so the PC itself never sees those keys) and
// sends the pressed-button mask here over localhost UDP.

namespace
{
constexpr uint16_t kKeypadPort = 8003;
constexpr uint32_t kKeypadMagic = 0x31504B46; // "FKP1"
constexpr uint32_t kDesktopPanelMagic = 0x31504446; // "FDP1": Desktop+ panel, see flow_shared_input.h
// Controller sits below and in front of the eyes, tilted up so its laser meets the gaze
// line this far ahead (about where the dashboard is).
constexpr float kControllerDown = 0.10f;
constexpr float kControllerForward = 0.15f;
constexpr float kGazeConvergeDistance = 2.0f;
} // namespace

FlowKeyboardMouseControllerDevice::FlowKeyboardMouseControllerDevice()
{
	DriverLog( "Flow keypad controller serial: %s", serial_number_.c_str() );
}

vr::EVRInitError FlowKeyboardMouseControllerDevice::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;
	is_active_ = true;

	const vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );

	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, model_number_.c_str() );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ManufacturerName_String, "FlowVR" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_TrackingSystemName_String, "flowvr" );
	vr::VRProperties()->SetUint64Property( container, vr::Prop_CurrentUniverseId_Uint64, kFlowTrackingUniverseId );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_RegisteredDeviceType_String, "flowvr/flow_keyboard_mouse_controller" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ControllerType_String, "flowvr_keyboard_mouse" );
	vr::VRProperties()->SetInt32Property( container, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_InputProfilePath_String, "{flowvr}/input/flowvr_keyboard_mouse_profile.json" );

	vr::VRDriverInput()->CreateScalarComponent( container, "/input/trigger/value", &input_handles_[ FlowKeyboardMouseComponent_trigger_value ],
		vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/trigger/click", &input_handles_[ FlowKeyboardMouseComponent_trigger_click ] );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/grip/click", &input_handles_[ FlowKeyboardMouseComponent_grip_click ] );
	vr::VRDriverInput()->CreateScalarComponent( container, "/input/trackpad/x", &input_handles_[ FlowKeyboardMouseComponent_trackpad_x ],
		vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided );
	vr::VRDriverInput()->CreateScalarComponent( container, "/input/trackpad/y", &input_handles_[ FlowKeyboardMouseComponent_trackpad_y ],
		vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/trackpad/touch", &input_handles_[ FlowKeyboardMouseComponent_trackpad_touch ] );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/trackpad/click", &input_handles_[ FlowKeyboardMouseComponent_trackpad_click ] );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/application_menu/click", &input_handles_[ FlowKeyboardMouseComponent_application_menu_click ] );
	// SteamVR reserves the system button for opening/closing the dashboard.
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/system/click", &input_handles_[ FlowKeyboardMouseComponent_system_click ] );
	vr::VRDriverInput()->CreateHapticComponent( container, "/output/haptic", &input_handles_[ FlowKeyboardMouseComponent_haptic ] );
	vr::VRDriverInput()->CreatePoseComponent( container, "/pose/raw", &input_handles_[ FlowKeyboardMouseComponent_pose_raw ] );
	vr::VRDriverInput()->CreatePoseComponent( container, "/pose/tip", &input_handles_[ FlowKeyboardMouseComponent_pose_tip ] );

	pose_update_thread_ = std::thread( &FlowKeyboardMouseControllerDevice::PoseUpdateThread, this );
	keypad_receive_thread_ = std::thread( &FlowKeyboardMouseControllerDevice::KeypadReceiveThread, this );
	DriverLog( "Flow keypad controller activated" );
	return vr::VRInitError_None;
}

void FlowKeyboardMouseControllerDevice::EnterStandby()
{
}

void *FlowKeyboardMouseControllerDevice::GetComponent( const char * )
{
	return nullptr;
}

void FlowKeyboardMouseControllerDevice::DebugRequest( const char *, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize > 0 )
	{
		pchResponseBuffer[ 0 ] = 0;
	}
}

vr::DriverPose_t FlowKeyboardMouseControllerDevice::GetPose()
{
	vr::DriverPose_t pose{};
	pose.qWorldFromDriverRotation.w = 1.0f;
	pose.qDriverFromHeadRotation.w = 1.0f;

	vr::TrackedDevicePose_t hmd_pose{};
	vr::VRServerDriverHost()->GetRawTrackedDevicePoses( 0.0f, &hmd_pose, 1 );

	const vr::HmdVector3_t hmd_position = HmdVector3_From34Matrix( hmd_pose.mDeviceToAbsoluteTracking );
	const vr::HmdQuaternion_t hmd_orientation = HmdQuaternion_FromMatrix( hmd_pose.mDeviceToAbsoluteTracking );
	const vr::HmdVector3_t offset_position = { 0.0f, -kControllerDown, -kControllerForward };
	const vr::HmdVector3_t position = hmd_position + ( offset_position * hmd_orientation );
	// Pitch up so the laser crosses the gaze line kGazeConvergeDistance ahead of the eyes.
	const double pitch_up = std::atan2( kControllerDown, kGazeConvergeDistance - kControllerForward );

	pose.qRotation = hmd_orientation * HmdQuaternion_FromEulerAngles( 0.0, pitch_up, 0.0 );
	pose.vecPosition[ 0 ] = position.v[ 0 ];
	pose.vecPosition[ 1 ] = position.v[ 1 ];
	pose.vecPosition[ 2 ] = position.v[ 2 ];
	if ( !pointer_gate_.Aiming() )
	{
		FlowPointerGate::Rest( pose );
	}
	// Step aside while the tracked right hand (an Index controller) has the keypad.
	const bool active = !g_right_hand_controller_active.load();
	pose.poseIsValid = active;
	pose.deviceIsConnected = active;
	pose.result = active ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
	return pose;
}

void FlowKeyboardMouseControllerDevice::PoseUpdateThread()
{
	while ( is_active_ )
	{
		const vr::TrackedDeviceIndex_t index = device_index_;
		if ( index != vr::k_unTrackedDeviceIndexInvalid )
		{
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated( index, GetPose(), sizeof( vr::DriverPose_t ) );
		}
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
}

void FlowKeyboardMouseControllerDevice::KeypadReceiveThread()
{
#ifdef _WIN32
	WSADATA wsa_data{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) != 0 )
	{
		DriverLog( "Flow keypad controller: WSAStartup failed" );
		return;
	}
	SOCKET sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons( kKeypadPort );
	address.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); // local helper only
	if ( sock == INVALID_SOCKET || bind( sock, reinterpret_cast< sockaddr * >( &address ), sizeof( address ) ) == SOCKET_ERROR )
	{
		DriverLog( "Flow keypad controller: bind 127.0.0.1:%u failed", kKeypadPort );
		if ( sock != INVALID_SOCKET )
		{
			closesocket( sock );
		}
		WSACleanup();
		return;
	}
	DWORD timeout_ms = 100; // lets the loop notice Deactivate
	setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast< const char * >( &timeout_ms ), sizeof( timeout_ms ) );
	DriverLog( "Flow keypad controller: listening on 127.0.0.1:%u", kKeypadPort );

	uint32_t last_logged = 0;
	while ( is_active_ )
	{
		// Keypad: magic, buttons[, status flags]. Desktop+ panel: magic, flags, 12 floats, width
		// [, 4 texture bounds[, curvature]].
		uint32_t packet[ 20 ] = {};
		const int received = recv( sock, reinterpret_cast< char * >( packet ), sizeof( packet ), 0 );
		const int words = received / static_cast< int >( sizeof( uint32_t ) );
		if ( ( words == 15 || words == 19 || words == 20 ) && received % sizeof( uint32_t ) == 0 && packet[ 0 ] == kDesktopPanelMagic )
		{
			FlowDesktopPanel panel;
			panel.visible = ( packet[ 1 ] & 1u ) != 0;
			panel.flags = packet[ 1 ];
			std::memcpy( panel.transform, &packet[ 2 ], sizeof( panel.transform ) );
			std::memcpy( &panel.width, &packet[ 14 ], sizeof( panel.width ) );
			if ( words >= 19 )
			{
				std::memcpy( panel.bounds, &packet[ 15 ], sizeof( panel.bounds ) );
			}
			if ( words >= 20 )
			{
				std::memcpy( &panel.curvature, &packet[ 19 ], sizeof( panel.curvature ) );
			}
			panel.updated_ms = FlowSteadyMilliseconds();
			g_desktop_panel.Store( panel );
			continue;
		}
		if ( received < static_cast< int >( 2 * sizeof( uint32_t ) ) || received > static_cast< int >( 3 * sizeof( uint32_t ) ) ||
		     packet[ 0 ] != kKeypadMagic )
		{
			continue;
		}
		g_keypad_buttons = packet[ 1 ];
		g_keypad_status = received == static_cast< int >( 3 * sizeof( uint32_t ) ) ? packet[ 2 ] : 0;
		g_keypad_updated_ms = FlowSteadyMilliseconds();
		if ( packet[ 1 ] != last_logged )
		{
			last_logged = packet[ 1 ];
			DriverLog( "Flow keypad controller: buttons=0x%03x", packet[ 1 ] );
		}
	}
	closesocket( sock );
	WSACleanup();
#endif
}

void FlowKeyboardMouseControllerDevice::Deactivate()
{
	if ( is_active_.exchange( false ) )
	{
		if ( pose_update_thread_.joinable() )
		{
			pose_update_thread_.join();
		}
		if ( keypad_receive_thread_.joinable() )
		{
			keypad_receive_thread_.join();
		}
	}
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void FlowKeyboardMouseControllerDevice::MyRunFrame()
{
	// While the right hand controller is active it gets the keypad; release everything here.
	uint32_t buttons = g_right_hand_controller_active.load() ? 0 : FlowKeypadButtons();
	pointer_gate_.Update( ( buttons & kFlowAimingButtons ) != 0 || pointer_gate_.TapPending() );
	buttons = ( buttons & ~kFlowAimingButtons ) | pointer_gate_.FilterButtons( buttons & kFlowAimingButtons );
	const auto pressed = [ buttons ]( uint32_t bit ) { return ( buttons & bit ) != 0; };

	const bool trigger = pressed( KeypadButton_Trigger );
	const bool trackpad_click = pressed( KeypadButton_TrackpadClick );
	const float trackpad_x = ( pressed( KeypadButton_Right ) ? 1.0f : 0.0f ) - ( pressed( KeypadButton_Left ) ? 1.0f : 0.0f );
	const float trackpad_y = ( pressed( KeypadButton_Up ) ? 1.0f : 0.0f ) - ( pressed( KeypadButton_Down ) ? 1.0f : 0.0f );
	const bool trackpad_touch = trackpad_click || trackpad_x != 0.0f || trackpad_y != 0.0f;

	vr::VRDriverInput()->UpdateBooleanComponent( input_handles_[ FlowKeyboardMouseComponent_trigger_click ], trigger, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( input_handles_[ FlowKeyboardMouseComponent_trigger_value ], trigger ? 1.0f : 0.0f, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( input_handles_[ FlowKeyboardMouseComponent_grip_click ], pressed( KeypadButton_Grip ), 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( input_handles_[ FlowKeyboardMouseComponent_trackpad_x ], trackpad_x, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( input_handles_[ FlowKeyboardMouseComponent_trackpad_y ], trackpad_y, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( input_handles_[ FlowKeyboardMouseComponent_trackpad_touch ], trackpad_touch, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( input_handles_[ FlowKeyboardMouseComponent_trackpad_click ], trackpad_click, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( input_handles_[ FlowKeyboardMouseComponent_application_menu_click ], pressed( KeypadButton_Menu ), 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( input_handles_[ FlowKeyboardMouseComponent_system_click ], pressed( KeypadButton_System ), 0.0 );
}

void FlowKeyboardMouseControllerDevice::MyProcessEvent( const vr::VREvent_t &vrevent )
{
	if ( vrevent.eventType == vr::VREvent_Input_HapticVibration
		&& vrevent.data.hapticVibration.componentHandle == input_handles_[ FlowKeyboardMouseComponent_haptic ] )
	{
		DriverLog( "Flow keypad controller haptic: duration=%.2f frequency=%.2f amplitude=%.2f",
			vrevent.data.hapticVibration.fDurationSeconds,
			vrevent.data.hapticVibration.fFrequency,
			vrevent.data.hapticVibration.fAmplitude );
	}
}

const std::string &FlowKeyboardMouseControllerDevice::MyGetSerialNumber() const
{
	return serial_number_;
}
