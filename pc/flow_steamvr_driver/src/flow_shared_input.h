#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Input shared between the devices of driver_flowvr: the numeric keypad (received by the keypad
// controller from flow_dashboard_helper) and the Flow's tracked hands (received by the HMD's
// pose thread, used by the hand controllers).

// ---- Numeric keypad -------------------------------------------------------------------------

// Bits of the mask sent by the helper (keep in sync with flow_dashboard_helper/main.cpp).
enum KeypadButton : uint32_t
{
	KeypadButton_Trigger = 1u << 0,      // keypad 5
	KeypadButton_Grip = 1u << 1,         // keypad 0 / Ins
	KeypadButton_TrackpadClick = 1u << 2, // keypad Enter
	KeypadButton_Up = 1u << 3,           // keypad + or 8
	KeypadButton_Down = 1u << 4,         // keypad - or 2
	KeypadButton_Left = 1u << 5,         // keypad 4
	KeypadButton_Right = 1u << 6,        // keypad 6
	KeypadButton_System = 1u << 7,       // keypad *
	KeypadButton_Menu = 1u << 8,         // keypad /
};

// Keypad buttons that act on what the laser points at (all but System and Menu).
constexpr uint32_t kFlowAimingButtons = KeypadButton_Trigger | KeypadButton_Grip | KeypadButton_TrackpadClick |
	KeypadButton_Up | KeypadButton_Down | KeypadButton_Left | KeypadButton_Right;

inline std::atomic< uint32_t > g_keypad_buttons{ 0 };
inline std::atomic< int64_t > g_keypad_updated_ms{ 0 }; // steady clock

inline int64_t FlowSteadyMilliseconds()
{
	return std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// Pressed keypad buttons, or none if the helper has been silent for a while (it repeats its
// state every 100 ms, so a closed helper cannot leave a button stuck down).
inline uint32_t FlowKeypadButtons()
{
	return FlowSteadyMilliseconds() - g_keypad_updated_ms.load() < 500 ? g_keypad_buttons.load() : 0;
}

// Status flags the helper sends along with the buttons (keep in sync with flow_dashboard_helper).
enum KeypadStatus : uint32_t
{
	KeypadStatus_DashboardVisible = 1u << 0,
	KeypadStatus_HandsDisabled = 1u << 1, // NumLock: ignore the Flow's hands (keypad pointer only)
};

inline std::atomic< uint32_t > g_keypad_status{ 0 };

// The SteamVR dashboard is open (as last reported by the helper; false if it went silent).
inline bool FlowDashboardVisible()
{
	return FlowSteadyMilliseconds() - g_keypad_updated_ms.load() < 500 &&
	       ( g_keypad_status.load() & KeypadStatus_DashboardVisible ) != 0;
}

// Hand tracking switched off with NumLock (on again if the helper goes silent).
inline bool FlowHandsDisabled()
{
	return FlowSteadyMilliseconds() - g_keypad_updated_ms.load() < 500 &&
	       ( g_keypad_status.load() & KeypadStatus_HandsDisabled ) != 0;
}

// True while the right hand controller is connected: the keypad's buttons then go to it and
// the head-aimed keypad controller steps aside.
inline std::atomic< bool > g_right_hand_controller_active{ false };

// ---- Desktop+ panel ------------------------------------------------------------------------

// Where Desktop+ draws its desktop panel (sent by the helper while the dashboard shows it), so
// the Flow can put its sharp desktop layer exactly there. Transform: row-major 3x4 in the
// driver's raw tracking space, including Desktop+'s scale; width in metres.
struct FlowDesktopPanel
{
	bool visible = false;
	uint32_t flags = 0; // as sent by the helper (bit 0 = show, bit 1 = keypad reticle)
	float transform[ 12 ] = {};
	float width = 0.0f;
	// Part of Desktop+'s texture the panel shows (uMin, uMax, vMin, vMax; v down). The texture
	// holds all monitors as laid out on the virtual desktop, so this tells which monitor it is.
	// All zero = unknown (older helper): the primary monitor.
	float bounds[ 4 ] = {};
	// SteamVR overlay curvature: width / (2 pi radius), bent towards the viewer; 0 = flat.
	float curvature = 0.0f;
	int64_t updated_ms = 0;
};

class FlowDesktopPanelStore
{
public:
	void Store( const FlowDesktopPanel &panel )
	{
		std::lock_guard< std::mutex > lock( mutex_ );
		panel_ = panel;
	}

	// Hidden once the helper has been silent for a while (it sends every 100 ms).
	FlowDesktopPanel Get() const
	{
		std::lock_guard< std::mutex > lock( mutex_ );
		FlowDesktopPanel panel = panel_;
		if ( FlowSteadyMilliseconds() - panel.updated_ms > 500 )
		{
			panel.visible = false;
		}
		return panel;
	}

private:
	mutable std::mutex mutex_;
	FlowDesktopPanel panel_;
};

inline FlowDesktopPanelStore g_desktop_panel;

// ---- Tracked hands --------------------------------------------------------------------------

// Wave natural hand tracker joints (WVR_HandJoint order).
enum FlowHandJoint
{
	FlowHandJoint_Palm = 0,
	FlowHandJoint_Wrist = 1,
	FlowHandJoint_ThumbBase = 2,   // thumb joints 2..5, tip last
	FlowHandJoint_IndexBase = 6,   // each finger: joint0..joint3, tip
	FlowHandJoint_MiddleBase = 11,
	FlowHandJoint_RingBase = 16,
	FlowHandJoint_PinkyBase = 21,
	FlowHandJoint_Count = 26,
};

enum FlowHandSide
{
	FlowHand_Left = 0,
	FlowHand_Right = 1,
};

// One hand from the Flow's "FLH1" packet, in the Flow's head-origin space (as the HMD pose,
// before the driver's standing-height offset).
struct FlowHandSample
{
	bool valid = false;
	float pinch = 0.0f; // index-finger pinch strength 0..1
	float joints[ FlowHandJoint_Count ][ 3 ] = {};
	std::chrono::steady_clock::time_point received_at{};
};

class FlowHandStore
{
public:
	void Store( FlowHandSide side, const FlowHandSample &sample )
	{
		std::lock_guard< std::mutex > lock( mutex_ );
		hands_[ side ] = sample;
	}

	FlowHandSample Get( FlowHandSide side ) const
	{
		std::lock_guard< std::mutex > lock( mutex_ );
		return hands_[ side ];
	}

private:
	mutable std::mutex mutex_;
	FlowHandSample hands_[ 2 ];
};

inline FlowHandStore g_flow_hands;
