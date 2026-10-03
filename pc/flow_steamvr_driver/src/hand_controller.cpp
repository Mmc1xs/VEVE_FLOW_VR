#include "hand_controller.h"

#include "driverlog.h"
#include "flow_pose_sync.h"
#include "vrmath.h"

#include <algorithm>
#include <cmath>

namespace
{
// A hand sample older than this is not tracked any more (the Flow sends one per frame).
constexpr int64_t kSampleTimeoutMs = 150;
// The controller stays connected this long after its hand was last tracked.
constexpr int64_t kDisconnectAfterMs = 3000;
constexpr float kPi = 3.14159265f;
// Pinch / fist strength that brings the laser back while the dashboard is open.
constexpr float kAimPinch = 0.5f;
constexpr float kAimGrip = 0.5f;

struct Vec3
{
	float x, y, z;
};

Vec3 operator-( const Vec3 &a, const Vec3 &b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator*( const Vec3 &a, float s ) { return { a.x * s, a.y * s, a.z * s }; }
float Dot( const Vec3 &a, const Vec3 &b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross( const Vec3 &a, const Vec3 &b ) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec3 Normalize( const Vec3 &a )
{
	const float length = std::sqrt( Dot( a, a ) );
	return length > 1e-6f ? a * ( 1.0f / length ) : Vec3{ 0.0f, 0.0f, 0.0f };
}

Vec3 Joint( const FlowHandSample &sample, int joint )
{
	return { sample.joints[ joint ][ 0 ], sample.joints[ joint ][ 1 ], sample.joints[ joint ][ 2 ] };
}

bool IsFresh( const FlowHandSample &sample )
{
	return sample.valid
		&& std::chrono::steady_clock::now() - sample.received_at < std::chrono::milliseconds( kSampleTimeoutMs );
}

// 0 = straight, 1 = fully curled: angle between the metacarpal (joint0 -> joint1) and the
// rest of the finger (joint1 -> tip).
float FingerCurl( const FlowHandSample &sample, int base_joint )
{
	const Vec3 metacarpal = Normalize( Joint( sample, base_joint + 1 ) - Joint( sample, base_joint ) );
	const Vec3 finger = Normalize( Joint( sample, base_joint + 4 ) - Joint( sample, base_joint + 1 ) );
	const float angle = std::acos( std::clamp( Dot( metacarpal, finger ), -1.0f, 1.0f ) );
	return std::clamp( angle / ( 0.75f * kPi ), 0.0f, 1.0f );
}

float Remap( float value, float from, float to )
{
	return std::clamp( ( value - from ) / ( to - from ), 0.0f, 1.0f );
}
} // namespace

FlowHandControllerDevice::FlowHandControllerDevice( FlowHandSide side )
	: side_( side ), serial_number_( side == FlowHand_Left ? "FLOW-HAND-LEFT" : "FLOW-HAND-RIGHT" )
{
}

vr::EVRInitError FlowHandControllerDevice::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;
	is_active_ = true;
	const bool left = side_ == FlowHand_Left;
	const vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( unObjectId );

	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, left ? "Knuckles Left" : "Knuckles Right" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_RenderModelName_String,
		left ? "{indexcontroller}valve_controller_knu_1_0_left" : "{indexcontroller}valve_controller_knu_1_0_right" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ManufacturerName_String, "Valve" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_TrackingSystemName_String, "flowvr" );
	vr::VRProperties()->SetUint64Property( container, vr::Prop_CurrentUniverseId_Uint64, kFlowTrackingUniverseId );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_RegisteredDeviceType_String, left ? "flowvr/flow_hand_left" : "flowvr/flow_hand_right" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ControllerType_String, "knuckles" );
	vr::VRProperties()->SetInt32Property( container, vr::Prop_ControllerRoleHint_Int32,
		left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand );
	// Preferred over the keypad controller for the right hand.
	vr::VRProperties()->SetInt32Property( container, vr::Prop_ControllerHandSelectionPriority_Int32, 100 );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_InputProfilePath_String, "{indexcontroller}/input/index_controller_profile.json" );

	const auto boolean = [ & ]( const char *path, FlowHandComponent component ) {
		vr::VRDriverInput()->CreateBooleanComponent( container, path, &input_handles_[ component ] );
	};
	const auto scalar = [ & ]( const char *path, FlowHandComponent component, vr::EVRScalarUnits units ) {
		vr::VRDriverInput()->CreateScalarComponent( container, path, &input_handles_[ component ], vr::VRScalarType_Absolute, units );
	};
	boolean( "/input/system/click", FlowHandComponent_system_click );
	boolean( "/input/system/touch", FlowHandComponent_system_touch );
	boolean( "/input/a/click", FlowHandComponent_a_click );
	boolean( "/input/a/touch", FlowHandComponent_a_touch );
	boolean( "/input/b/click", FlowHandComponent_b_click );
	boolean( "/input/b/touch", FlowHandComponent_b_touch );
	boolean( "/input/trigger/click", FlowHandComponent_trigger_click );
	boolean( "/input/trigger/touch", FlowHandComponent_trigger_touch );
	scalar( "/input/trigger/value", FlowHandComponent_trigger_value, vr::VRScalarUnits_NormalizedOneSided );
	boolean( "/input/grip/touch", FlowHandComponent_grip_touch );
	scalar( "/input/grip/force", FlowHandComponent_grip_force, vr::VRScalarUnits_NormalizedOneSided );
	scalar( "/input/grip/value", FlowHandComponent_grip_value, vr::VRScalarUnits_NormalizedOneSided );
	boolean( "/input/thumbstick/click", FlowHandComponent_thumbstick_click );
	boolean( "/input/thumbstick/touch", FlowHandComponent_thumbstick_touch );
	scalar( "/input/thumbstick/x", FlowHandComponent_thumbstick_x, vr::VRScalarUnits_NormalizedTwoSided );
	scalar( "/input/thumbstick/y", FlowHandComponent_thumbstick_y, vr::VRScalarUnits_NormalizedTwoSided );
	scalar( "/input/finger/index", FlowHandComponent_finger_index, vr::VRScalarUnits_NormalizedOneSided );
	scalar( "/input/finger/middle", FlowHandComponent_finger_middle, vr::VRScalarUnits_NormalizedOneSided );
	scalar( "/input/finger/ring", FlowHandComponent_finger_ring, vr::VRScalarUnits_NormalizedOneSided );
	scalar( "/input/finger/pinky", FlowHandComponent_finger_pinky, vr::VRScalarUnits_NormalizedOneSided );
	vr::VRDriverInput()->CreateHapticComponent( container, "/output/haptic", &input_handles_[ FlowHandComponent_haptic ] );

	// Rotates the hand frame into the controller's; tune with driver_flowvr.hand_pitch_offset_deg.
	pitch_offset_rad_ = vr::VRSettings()->GetFloat( "driver_flowvr", "hand_pitch_offset_deg" ) * kPi / 180.0f;

	pose_update_thread_ = std::thread( &FlowHandControllerDevice::PoseUpdateThread, this );
	DriverLog( "Flow hand controller activated: %s", serial_number_.c_str() );
	return vr::VRInitError_None;
}

void FlowHandControllerDevice::EnterStandby()
{
}

void *FlowHandControllerDevice::GetComponent( const char * )
{
	return nullptr;
}

void FlowHandControllerDevice::DebugRequest( const char *, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize > 0 )
	{
		pchResponseBuffer[ 0 ] = 0;
	}
}

// Controller frame from the hand: -Z along the hand (wrist -> middle knuckle), X towards the
// little finger side for the right hand / thumb side for the left (both "right" with the palm
// down), Y out of the back of the hand. Origin at the palm centre.
vr::DriverPose_t FlowHandControllerDevice::GetPose()
{
	vr::DriverPose_t pose{};
	pose.qWorldFromDriverRotation.w = 1.0f;
	pose.qDriverFromHeadRotation.w = 1.0f;
	pose.qRotation.w = 1.0f;
	pose.deviceIsConnected = connected_.load();

	const FlowHandSample sample = g_flow_hands.Get( side_ );
	if ( !IsFresh( sample ) )
	{
		pose.result = pose.deviceIsConnected ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Uninitialized;
		return pose;
	}

	const Vec3 forward = Normalize( Joint( sample, FlowHandJoint_MiddleBase + 1 ) - Joint( sample, FlowHandJoint_Wrist ) );
	const Vec3 across = Normalize( Joint( sample, FlowHandJoint_IndexBase + 1 ) - Joint( sample, FlowHandJoint_PinkyBase + 1 ) );
	const Vec3 z = forward * -1.0f;
	const Vec3 right = side_ == FlowHand_Right ? across * -1.0f : across;
	const Vec3 y = Normalize( Cross( z, right ) );
	const Vec3 x = Cross( y, z );

	vr::HmdMatrix33_t rotation{};
	rotation.m[ 0 ][ 0 ] = x.x; rotation.m[ 0 ][ 1 ] = y.x; rotation.m[ 0 ][ 2 ] = z.x;
	rotation.m[ 1 ][ 0 ] = x.y; rotation.m[ 1 ][ 1 ] = y.y; rotation.m[ 1 ][ 2 ] = z.y;
	rotation.m[ 2 ][ 0 ] = x.z; rotation.m[ 2 ][ 1 ] = y.z; rotation.m[ 2 ][ 2 ] = z.z;
	pose.qRotation = HmdQuaternion_Normalize( HmdQuaternion_FromMatrix( rotation ) )
		* HmdQuaternion_FromEulerAngles( 0.0, pitch_offset_rad_, 0.0 );

	const Vec3 palm = Joint( sample, FlowHandJoint_Palm );
	pose.vecPosition[ 0 ] = palm.x;
	pose.vecPosition[ 1 ] = palm.y + kFlowStandingHeightOffset;
	pose.vecPosition[ 2 ] = palm.z;
	if ( !pointer_gate_.Aiming() )
	{
		FlowPointerGate::Rest( pose );
	}
	pose.poseIsValid = true;
	pose.result = vr::TrackingResult_Running_OK;
	return pose;
}

void FlowHandControllerDevice::UpdateConnected( bool tracked )
{
	const int64_t now = FlowSteadyMilliseconds();
	if ( tracked )
	{
		last_tracked_ms_ = now;
	}
	const bool connected = tracked || ( connected_.load() && now - last_tracked_ms_.load() < kDisconnectAfterMs );
	if ( connected_.exchange( connected ) != connected )
	{
		DriverLog( "Flow hand controller %s %s", serial_number_.c_str(), connected ? "connected" : "disconnected" );
	}
	if ( side_ == FlowHand_Right )
	{
		g_right_hand_controller_active = connected;
	}
}

void FlowHandControllerDevice::PoseUpdateThread()
{
	while ( is_active_ )
	{
		UpdateConnected( IsFresh( g_flow_hands.Get( side_ ) ) );
		const vr::TrackedDeviceIndex_t index = device_index_;
		if ( index != vr::k_unTrackedDeviceIndexInvalid )
		{
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated( index, GetPose(), sizeof( vr::DriverPose_t ) );
		}
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
}

void FlowHandControllerDevice::Deactivate()
{
	if ( is_active_.exchange( false ) && pose_update_thread_.joinable() )
	{
		pose_update_thread_.join();
	}
	if ( side_ == FlowHand_Right )
	{
		g_right_hand_controller_active = false;
	}
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void FlowHandControllerDevice::MyRunFrame()
{
	if ( device_index_ == vr::k_unTrackedDeviceIndexInvalid )
	{
		return;
	}
	const FlowHandSample sample = g_flow_hands.Get( side_ );
	const bool tracked = IsFresh( sample );

	float pinch = 0.0f;
	float curl_index = 0.0f, curl_middle = 0.0f, curl_ring = 0.0f, curl_pinky = 0.0f;
	if ( tracked )
	{
		pinch = sample.pinch;
		curl_index = FingerCurl( sample, FlowHandJoint_IndexBase );
		curl_middle = FingerCurl( sample, FlowHandJoint_MiddleBase );
		curl_ring = FingerCurl( sample, FlowHandJoint_RingBase );
		curl_pinky = FingerCurl( sample, FlowHandJoint_PinkyBase );
	}
	const float fist = ( curl_middle + curl_ring + curl_pinky ) / 3.0f;
	float grip_value = Remap( fist, 0.25f, 0.75f );
	float grip_force = Remap( fist, 0.75f, 1.0f );

	// The right hand takes over the keypad while it is connected.
	uint32_t keys = side_ == FlowHand_Right && connected_.load() ? FlowKeypadButtons() : 0;
	const auto pressed = [ &keys ]( uint32_t bit ) { return ( keys & bit ) != 0; };
	if ( pressed( KeypadButton_Trigger ) )
	{
		pinch = 1.0f;
	}
	if ( pressed( KeypadButton_Grip ) )
	{
		grip_value = 1.0f;
		grip_force = 1.0f;
	}
	// While the dashboard is open the laser only reaches the overlays during a pinch, a fist or
	// a keypad press (see FlowPointerGate); the first moments of each are held back.
	pointer_gate_.Update( pinch > kAimPinch || grip_value > kAimGrip || ( keys & kFlowAimingButtons ) != 0 );
	if ( !pointer_gate_.ClickAllowed() )
	{
		pinch = 0.0f;
		grip_value = 0.0f;
		grip_force = 0.0f;
		keys &= ~kFlowAimingButtons;
	}
	// Click with hysteresis so a pinch held near the threshold does not chatter.
	trigger_clicked_ = trigger_clicked_ ? pinch > 0.6f : pinch > 0.85f;
	const float stick_x = ( pressed( KeypadButton_Right ) ? 1.0f : 0.0f ) - ( pressed( KeypadButton_Left ) ? 1.0f : 0.0f );
	const float stick_y = ( pressed( KeypadButton_Up ) ? 1.0f : 0.0f ) - ( pressed( KeypadButton_Down ) ? 1.0f : 0.0f );

	vr::IVRDriverInput *input = vr::VRDriverInput();
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_system_click ], pressed( KeypadButton_System ), 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_system_touch ], pressed( KeypadButton_System ), 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_a_click ], pressed( KeypadButton_TrackpadClick ), 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_a_touch ], pressed( KeypadButton_TrackpadClick ), 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_b_click ], pressed( KeypadButton_Menu ), 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_b_touch ], pressed( KeypadButton_Menu ), 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_trigger_click ], trigger_clicked_, 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_trigger_touch ], pinch > 0.2f || curl_index > 0.3f, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_trigger_value ], pinch, 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_grip_touch ], grip_value > 0.1f, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_grip_force ], grip_force, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_grip_value ], grip_value, 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_thumbstick_click ], false, 0.0 );
	input->UpdateBooleanComponent( input_handles_[ FlowHandComponent_thumbstick_touch ], stick_x != 0.0f || stick_y != 0.0f, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_thumbstick_x ], stick_x, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_thumbstick_y ], stick_y, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_finger_index ], curl_index, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_finger_middle ], curl_middle, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_finger_ring ], curl_ring, 0.0 );
	input->UpdateScalarComponent( input_handles_[ FlowHandComponent_finger_pinky ], curl_pinky, 0.0 );

	const int64_t now = FlowSteadyMilliseconds();
	if ( tracked && now - last_input_log_ms_ >= 2000 )
	{
		last_input_log_ms_ = now;
		DriverLog( "Flow hand %s: pinch=%.2f trigger=%d curl index=%.2f middle=%.2f ring=%.2f pinky=%.2f grip=%.2f keys=0x%03x",
			serial_number_.c_str(), pinch, trigger_clicked_ ? 1 : 0, curl_index, curl_middle, curl_ring, curl_pinky, grip_value, keys );
	}
}

const std::string &FlowHandControllerDevice::MyGetSerialNumber() const
{
	return serial_number_;
}
