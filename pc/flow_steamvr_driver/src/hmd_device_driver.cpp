//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "hmd_device_driver.h"

#include "driverlog.h"
#include "flow_pose_sync.h"
#include "flow_shared_input.h"
#include "vrmath.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// Let's create some variables for strings used in getting settings.
// This is the section where all of the settings we want are stored. A section name can be anything,
// but if you want to store driver specific settings, it's best to namespace the section with the driver identifier
// ie "<my_driver>_<section>" to avoid collisions
static const char *my_hmd_main_settings_section = "driver_flowvr";
static const char *my_hmd_display_settings_section = "flowvr_display";

MyHMDControllerDeviceDriver::MyHMDControllerDeviceDriver()
{
	// Keep track of whether Activate() has been called
	is_active_ = false;

	// We have our model number and serial number stored in SteamVR settings. We need to get them and do so here.
	// Other IVRSettings methods (to get int32, floats, bools) return the data, instead of modifying, but strings are
	// different.
	char model_number[ 1024 ];
	vr::VRSettings()->GetString( my_hmd_main_settings_section, "model_number", model_number, sizeof( model_number ) );
	my_hmd_model_number_ = model_number;

	// Get our serial number depending on our "handedness"
	char serial_number[ 1024 ];
	vr::VRSettings()->GetString( my_hmd_main_settings_section, "serial_number", serial_number, sizeof( serial_number ) );
	my_hmd_serial_number_ = serial_number;

	// Here's an example of how to use our logging wrapper around IVRDriverLog
	// In SteamVR logs (SteamVR Hamburger Menu > Developer Settings > Web console) drivers have a prefix of
	// "<driver_name>:". You can search this in the top search bar to find the info that you've logged.
	DriverLog( "Flow HMD model number: %s", my_hmd_model_number_.c_str() );
	DriverLog( "Flow HMD serial number: %s", my_hmd_serial_number_.c_str() );

	// Display settings
	MyHMDDisplayDriverConfiguration display_configuration{};
	display_configuration.window_x = vr::VRSettings()->GetInt32( my_hmd_display_settings_section, "window_x" );
	display_configuration.window_y = vr::VRSettings()->GetInt32( my_hmd_display_settings_section, "window_y" );

	display_configuration.window_width = vr::VRSettings()->GetInt32( my_hmd_display_settings_section, "window_width" );
	display_configuration.window_height = vr::VRSettings()->GetInt32( my_hmd_display_settings_section, "window_height" );

	display_configuration.render_width = vr::VRSettings()->GetInt32( my_hmd_display_settings_section, "render_width" );
	display_configuration.render_height = vr::VRSettings()->GetInt32( my_hmd_display_settings_section, "render_height" );

	display_configuration.tan_left = vr::VRSettings()->GetFloat( my_hmd_display_settings_section, "tan_left" );
	display_configuration.tan_right = vr::VRSettings()->GetFloat( my_hmd_display_settings_section, "tan_right" );
	display_configuration.tan_top = vr::VRSettings()->GetFloat( my_hmd_display_settings_section, "tan_top" );
	display_configuration.tan_bottom = vr::VRSettings()->GetFloat( my_hmd_display_settings_section, "tan_bottom" );
	if ( !( display_configuration.tan_left < 0.0f && display_configuration.tan_right > 0.0f &&
	        display_configuration.tan_top > 0.0f && display_configuration.tan_bottom < 0.0f ) )
	{
		// Measured on VIVE Flow: symmetric 93.5 degree frustum.
		display_configuration.tan_left = -1.0639f;
		display_configuration.tan_right = 1.0639f;
		display_configuration.tan_top = 1.0639f;
		display_configuration.tan_bottom = -1.0639f;
	}
	DriverLog( "Flow HMD display render=%dx%d window=%dx%d tan l=%.4f r=%.4f t=%.4f b=%.4f",
	           display_configuration.render_width, display_configuration.render_height,
	           display_configuration.window_width, display_configuration.window_height,
	           display_configuration.tan_left, display_configuration.tan_right,
	           display_configuration.tan_top, display_configuration.tan_bottom );

	// Instantiate our display component
	my_display_component_ = std::make_unique< MyHMDDisplayComponent >( display_configuration );
	my_virtual_display_component_ = std::make_unique< FlowVirtualDisplayDevice >();
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver after our
//  IServerTrackedDeviceProvider calls IVRServerDriverHost::TrackedDeviceAdded.
//-----------------------------------------------------------------------------
vr::EVRInitError MyHMDControllerDeviceDriver::Activate( uint32_t unObjectId )
{
	// Let's keep track of our device index. It'll be useful later.
	// Also, if we re-activate, be sure to set this.
	device_index_ = unObjectId;

	// Set a member to keep track of whether we've activated yet or not
	is_active_ = true;

	// For keeping track of frame number for animating motion.
	frame_number_ = 0;

	// Properties are stored in containers, usually one container per device index. We need to get this container to set
	// The properties we want, so we call this to retrieve a handle to it.
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );

	// Let's begin setting up the properties now we've got our container.
	// A list of properties available is contained in vr::ETrackedDeviceProperty.

	// First, let's set the model number.
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, my_hmd_model_number_.c_str() );

	// Next, display settings

	// Use the Flow's own IPD (WVR_GetTransformFromEyeToHead) so stereo matches the headset.
	float ipd = vr::VRSettings()->GetFloat( my_hmd_display_settings_section, "ipd_meters" );
	if ( ipd < 0.04f || ipd > 0.09f )
	{
		ipd = vr::VRSettings()->GetFloat( vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float );
	}
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, ipd );
	DriverLog( "Flow HMD IPD %.4f m", ipd );

	// For HMDs, it's required that a refresh rate is set otherwise VRCompositor will fail to start.
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, 75.f );

	// The distance from the user's eyes to the display in meters. This is used for reprojection.
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f );

	// How long from the compositor to submit a frame to the time it takes to display it on the screen.
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float,
	                                      vr::VRSettings()->GetFloat( my_hmd_display_settings_section, "vsync_to_photons" ) );

	// avoid "not fullscreen" warnings from vrmonitor
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, false );

	// User presence comes from the /proximity input (see MyRunFrame).
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_ContainsProximitySensor_Bool, true );

	vr::VRProperties()->SetUint64Property( container, vr::Prop_CurrentUniverseId_Uint64, kFlowTrackingUniverseId );

	// Now let's set up our inputs
	// This tells the UI what to show the user for bindings for this controller,
	// As well as what default bindings should be for legacy apps.
	// Note, we can use the wildcard {<driver_name>} to match the root folder location
	// of our driver.
	vr::VRProperties()->SetStringProperty( container, vr::Prop_InputProfilePath_String, "{flowvr}/input/mysimplehmd_profile.json" );

	// Let's set up handles for all of our components.
	// Even though these are also defined in our input profile,
	// We need to get handles to them to update the inputs.
	vr::VRDriverInput()->CreateBooleanComponent( container, "/proximity", &my_input_handles_[ MyComponent_head_proximity_click ] );

	my_pose_update_thread_ = std::thread( &MyHMDControllerDeviceDriver::MyPoseUpdateThread, this );
	my_pose_receive_thread_ = std::thread( &MyHMDControllerDeviceDriver::MyPoseReceiveThread, this );

	// We've activated everything successfully!
	// Let's tell SteamVR that by saying we don't have any errors.
	return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: If you're an HMD, this is where you would return an implementation
// of vr::IVRDisplayComponent, vr::IVRVirtualDisplay or vr::IVRDirectModeComponent.
//-----------------------------------------------------------------------------
void *MyHMDControllerDeviceDriver::GetComponent( const char *pchComponentNameAndVersion )
{
	if ( strcmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) == 0 )
	{
		return my_display_component_.get();
	}
	if ( strcmp( pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version ) == 0 )
	{
		DriverLog( "Flow HMD virtual display component requested: %s", pchComponentNameAndVersion );
		return static_cast< vr::IVRVirtualDisplay * >( my_virtual_display_component_.get() );
	}

	return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when a debug request has been made from an application to the driver.
// What is in the response and request is up to the application and driver to figure out themselves.
//-----------------------------------------------------------------------------
void MyHMDControllerDeviceDriver::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize >= 1 )
		pchResponseBuffer[ 0 ] = 0;
}

//-----------------------------------------------------------------------------
// Purpose: This is never called by vrserver in recent OpenVR versions,
// but is useful for giving data to vr::VRServerDriverHost::TrackedDevicePoseUpdated.
//-----------------------------------------------------------------------------
vr::DriverPose_t MyHMDControllerDeviceDriver::GetPose()
{
	// Let's retrieve the Hmd pose to base our controller pose off.

	// First, initialize the struct that we'll be submitting to the runtime to tell it we've updated our pose.
	vr::DriverPose_t pose = { 0 };

	// These need to be set to be valid quaternions. The device won't appear otherwise.
	pose.qWorldFromDriverRotation.w = 1.f;
	pose.qDriverFromHeadRotation.w = 1.f;

	FlowPose flow_pose;
	{
		std::lock_guard< std::mutex > lock( pose_mutex_ );
		flow_pose = latest_pose_;
	}

	const bool flow_pose_fresh = IsFlowPoseFresh( flow_pose, std::chrono::steady_clock::now() );
	g_flow_pose_sequence_in_use.store( flow_pose_fresh ? flow_pose.sequence : 0 );

	pose.qRotation.x = flow_pose_fresh ? flow_pose.qx : 0.0;
	pose.qRotation.y = flow_pose_fresh ? flow_pose.qy : 0.0;
	pose.qRotation.z = flow_pose_fresh ? flow_pose.qz : 0.0;
	pose.qRotation.w = flow_pose_fresh ? flow_pose.qw : 1.0;

	pose.vecPosition[ 0 ] = flow_pose_fresh ? flow_pose.x : 0.0;
	pose.vecPosition[ 1 ] = ( flow_pose_fresh ? flow_pose.y : 0.0 ) + kFlowStandingHeightOffset;
	pose.vecPosition[ 2 ] = flow_pose_fresh ? flow_pose.z : 0.0;

	// The pose we provided is valid.
	// This should be set is
	pose.poseIsValid = true;

	// Our device is always connected.
	// In reality with physical devices, when they get disconnected,
	// set this to false and icons in SteamVR will be updated to show the device is disconnected
	pose.deviceIsConnected = true;

	// The state of our tracking. For our virtual device, it's always going to be ok,
	// but this can get set differently to inform the runtime about the state of the device's tracking
	// and update the icons to inform the user accordingly.
	pose.result = vr::TrackingResult_Running_OK;

	// For HMDs we want to apply rotation/motion prediction
	pose.shouldApplyHeadModel = true;

	return pose;
}

bool MyHMDControllerDeviceDriver::IsFlowPoseFresh( const FlowPose &pose, std::chrono::steady_clock::time_point now ) const
{
	return pose.received_at != ( std::chrono::steady_clock::time_point::min )() // parens dodge the windows.h min macro
		&& now - pose.received_at < std::chrono::milliseconds( 500 );
}

void MyHMDControllerDeviceDriver::MyPoseReceiveThread()
{
#ifdef _WIN32
	WSADATA wsa_data{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) != 0 )
	{
		DriverLog( "Flow pose UDP: WSAStartup failed" );
		return;
	}

	SOCKET socket_handle = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if ( socket_handle == INVALID_SOCKET )
	{
		DriverLog( "Flow pose UDP: socket creation failed" );
		WSACleanup();
		return;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl( INADDR_ANY );
	address.sin_port = htons( 8002 );

	if ( bind( socket_handle, reinterpret_cast< sockaddr * >( &address ), sizeof( address ) ) == SOCKET_ERROR )
	{
		DriverLog( "Flow pose UDP: bind 0.0.0.0:8002 failed" );
		closesocket( socket_handle );
		WSACleanup();
		return;
	}

	DWORD timeout_ms = 100;
	setsockopt( socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast< const char * >( &timeout_ms ), sizeof( timeout_ms ) );
	DriverLog( "Flow pose UDP: listening on 0.0.0.0:8002" );

#pragma pack(push, 1)
	struct Packet
	{
		uint32_t magic;
		uint32_t sequence;
		float x;
		float y;
		float z;
		float qx;
		float qy;
		float qz;
		float qw;
	};
	// Both hands, sent by the Flow every frame while hand tracking runs (head-origin space).
	struct HandPacket
	{
		uint32_t magic; // "FLH1"
		uint32_t sequence;
		uint8_t valid[ 2 ]; // [0] left, [1] right
		uint8_t reserved[ 2 ];
		float pinch[ 2 ];
		float joints[ 2 ][ FlowHandJoint_Count ][ 3 ];
	};
#pragma pack(pop)
	constexpr uint32_t kHandPacketMagic = 0x31484C46;

	while ( is_active_ )
	{
		char buffer[ 1500 ];
		sockaddr_in from{};
		int from_len = sizeof( from );
		const int received = recvfrom( socket_handle, buffer, sizeof( buffer ), 0,
		                               reinterpret_cast< sockaddr * >( &from ), &from_len );
		if ( received == sizeof( HandPacket ) && *reinterpret_cast< const uint32_t * >( buffer ) == kHandPacketMagic )
		{
			HandPacket hands{};
			std::memcpy( &hands, buffer, sizeof( hands ) );
			const auto now = std::chrono::steady_clock::now();
			for ( int side = 0; side < 2; ++side )
			{
				FlowHandSample sample{};
				sample.valid = hands.valid[ side ] != 0;
				sample.pinch = hands.pinch[ side ];
				std::memcpy( sample.joints, hands.joints[ side ], sizeof( sample.joints ) );
				sample.received_at = now;
				g_flow_hands.Store( static_cast< FlowHandSide >( side ), sample );
			}
			continue;
		}
		Packet packet{};
		if ( received != sizeof( packet ) )
		{
			continue;
		}
		std::memcpy( &packet, buffer, sizeof( packet ) );
		if ( packet.magic != 0x31504C46 )
		{
			continue;
		}

		const float norm = std::sqrt( packet.qx * packet.qx + packet.qy * packet.qy
		                              + packet.qz * packet.qz + packet.qw * packet.qw );
		if ( norm < 0.001f )
		{
			continue;
		}

		FlowPose pose{};
		pose.x = packet.x;
		pose.y = packet.y;
		pose.z = packet.z;
		pose.qx = packet.qx / norm;
		pose.qy = packet.qy / norm;
		pose.qz = packet.qz / norm;
		pose.qw = packet.qw / norm;
		pose.sequence = packet.sequence;
		pose.received_at = std::chrono::steady_clock::now();

		{
			std::lock_guard< std::mutex > lock( pose_mutex_ );
			latest_pose_ = pose;
		}
		if ( pose.sequence == 1 || ( pose.sequence % 300 ) == 0 )
		{
			DriverLog( "Flow pose UDP: seq=%u pos=%.3f,%.3f,%.3f quat=%.3f,%.3f,%.3f,%.3f",
			           pose.sequence, pose.x, pose.y, pose.z, pose.qx, pose.qy, pose.qz, pose.qw );
		}
	}

	closesocket( socket_handle );
	WSACleanup();
#endif
}

void MyHMDControllerDeviceDriver::MyPoseUpdateThread()
{
	while ( is_active_ )
	{
		// Inform the vrserver that our tracked device's pose has updated, giving it the pose returned by our GetPose().
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated( device_index_, GetPose(), sizeof( vr::DriverPose_t ) );

		// Update our pose every five milliseconds.
		// In reality, you should update the pose whenever you have new data from your device.
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when the device should enter standby mode.
// The device should be put into whatever low power mode it has.
// We don't really have anything to do here, so let's just log something.
//-----------------------------------------------------------------------------
void MyHMDControllerDeviceDriver::EnterStandby()
{
	DriverLog( "HMD has been put into standby." );
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when the device should deactivate.
// This is typically at the end of a session
// The device should free any resources it has allocated here.
//-----------------------------------------------------------------------------
void MyHMDControllerDeviceDriver::Deactivate()
{
	// Let's join our pose thread that's running
	// by first checking then setting is_active_ to false to break out
	// of the while loop, if it's running, then call .join() on the thread
	if ( is_active_.exchange( false ) )
	{
		if ( my_pose_update_thread_.joinable() )
		{
			my_pose_update_thread_.join();
		}
		if ( my_pose_receive_thread_.joinable() )
		{
			my_pose_receive_thread_.join();
		}
	}

	// unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}


//-----------------------------------------------------------------------------
// Purpose: This is called by our IServerTrackedDeviceProvider when its RunFrame() method gets called.
// It's not part of the ITrackedDeviceServerDriver interface, we created it ourselves.
//-----------------------------------------------------------------------------
void MyHMDControllerDeviceDriver::MyRunFrame()
{
	frame_number_++;

	// SteamVR derives "user present" from the proximity input. While it is false the HMD's
	// activity level stays Idle and apps such as Unity titles fade the scene to grey, so report
	// the headset as worn whenever the Flow is sending poses.
	FlowPose flow_pose;
	{
		std::lock_guard< std::mutex > lock( pose_mutex_ );
		flow_pose = latest_pose_;
	}
	const bool present = IsFlowPoseFresh( flow_pose, std::chrono::steady_clock::now() );
	if ( present != user_present_ || frame_number_ == 1 )
	{
		vr::VRDriverInput()->UpdateBooleanComponent( my_input_handles_[ MyComponent_head_proximity_click ], present, 0.0 );
		user_present_ = present;
		DriverLog( "Flow user presence (head proximity): %s", present ? "present" : "absent" );
	}
}


//-----------------------------------------------------------------------------
// Purpose: This is called by our IServerTrackedDeviceProvider when it pops an event off the event queue.
// It's not part of the ITrackedDeviceServerDriver interface, we created it ourselves.
//-----------------------------------------------------------------------------
void MyHMDControllerDeviceDriver::MyProcessEvent( const vr::VREvent_t &vrevent )
{
}


//-----------------------------------------------------------------------------
// Purpose: Our IServerTrackedDeviceProvider needs our serial number to add us to vrserver.
// It's not part of the ITrackedDeviceServerDriver interface, we created it ourselves.
//-----------------------------------------------------------------------------
const std::string &MyHMDControllerDeviceDriver::MyGetSerialNumber()
{
	return my_hmd_serial_number_;
}

//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

MyHMDDisplayComponent::MyHMDDisplayComponent( const MyHMDDisplayDriverConfiguration &config )
	: config_( config )
{
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor if this display is considered an on-desktop display.
//-----------------------------------------------------------------------------
bool MyHMDDisplayComponent::IsDisplayOnDesktop()
{
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: To as vrcompositor to search for this display.
//-----------------------------------------------------------------------------
bool MyHMDDisplayComponent::IsDisplayRealDisplay()
{
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform the rest of the vr system what the recommended target size should be
//-----------------------------------------------------------------------------
void MyHMDDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnWidth = config_.render_width;
	*pnHeight = config_.render_height;
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor how the screens should be organized.
//-----------------------------------------------------------------------------
void MyHMDDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnY = 0;

	// Each eye will have half the window
	*pnWidth = config_.window_width / 2;

	// Each eye will have the full height
	*pnHeight = config_.window_height;

	if ( eEye == vr::Eye_Left )
	{
		// Left eye viewport on the left half of the window
		*pnX = 0;
	}
	else
	{
		// Right eye viewport on the right half of the window
		*pnX = config_.window_width / 2;
	}
}

//-----------------------------------------------------------------------------
// Purpose: To inform the compositor what the projection parameters are for this HMD.
//-----------------------------------------------------------------------------
void MyHMDDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// OpenVR wants tangents with +Y pointing down, so top/bottom flip sign versus Wave.
	*pfLeft = config_.tan_left;
	*pfRight = config_.tan_right;
	*pfTop = -config_.tan_top;
	*pfBottom = -config_.tan_bottom;
}

//-----------------------------------------------------------------------------
// Purpose: To compute the distortion properties for a given uv in an image.
//-----------------------------------------------------------------------------
vr::DistortionCoordinates_t MyHMDDisplayComponent::ComputeDistortion( vr::EVREye eEye, float fU, float fV )
{
	vr::DistortionCoordinates_t coordinates{};
	coordinates.rfBlue[ 0 ] = fU;
	coordinates.rfBlue[ 1 ] = fV;
	coordinates.rfGreen[ 0 ] = fU;
	coordinates.rfGreen[ 1 ] = fV;
	coordinates.rfRed[ 0 ] = fU;
	coordinates.rfRed[ 1 ] = fV;
	return coordinates;
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor what the window bounds for this virtual HMD are.
//-----------------------------------------------------------------------------
void MyHMDDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnX = config_.window_x;
	*pnY = config_.window_y;
	*pnWidth = config_.window_width;
	*pnHeight = config_.window_height;
}

bool MyHMDDisplayComponent::ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV)
{
	//Return false to let SteamVR infer an estimate from ComputeDistortion
	return false;
}
