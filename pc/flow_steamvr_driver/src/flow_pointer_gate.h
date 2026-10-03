#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "flow_shared_input.h"
#include "openvr_driver.h"
#include "vrmath.h"

// While the SteamVR dashboard is open, Desktop+ moves the Windows cursor to wherever a laser
// rests on its panel; with a head-aimed laser that means the cursor follows the head. So while
// the dashboard is open our controllers only aim at the overlays while the user is pressing
// something: otherwise they rest well below the user with the laser straight down, away from
// every overlay and out of sight (a model left in place would block the view).
//
// Pressing brings the aim back first and holds the press back for kAimLeadMs, so Desktop+ has
// moved the cursor before the click lands. After the release the aim stays for kAimHoldMs so
// the release (mouse up) still lands on the panel.
class FlowPointerGate
{
public:
	// Called once per frame with whether the user is pressing something that should aim.
	void Update( bool engaged )
	{
		const int64_t now = FlowSteadyMilliseconds();
		if ( !FlowDashboardVisible() )
		{
			aiming_ = true;
			click_allowed_ = true;
			aim_since_ms_ = now - kAimLeadMs;
			return;
		}
		if ( engaged )
		{
			if ( !aiming_ )
			{
				aiming_ = true;
				aim_since_ms_ = now;
			}
			last_engaged_ms_ = now;
		}
		else if ( aiming_ && now - last_engaged_ms_ >= kAimHoldMs )
		{
			aiming_ = false;
		}
		click_allowed_ = aiming_ && now - aim_since_ms_ >= kAimLeadMs;
	}

	// Read by the pose thread.
	bool Aiming() const { return aiming_.load(); }
	// False while the aim has just come back: report the buttons that aim as released.
	bool ClickAllowed() const { return click_allowed_; }

	// Moves an aimed pose to the resting place: kRestDrop below, laser straight down.
	static void Rest( vr::DriverPose_t &pose )
	{
		const vr::HmdVector3_t forward = HmdVector3_Forward * pose.qRotation;
		const double yaw = std::atan2( -forward.v[ 0 ], -forward.v[ 2 ] );
		pose.qRotation = HmdQuaternion_FromEulerAngles( 0.0, 0.0, yaw ) * HmdQuaternion_FromEulerAngles( 0.0, -1.5707963267948966, 0.0 );
		pose.vecPosition[ 1 ] -= kRestDrop;
	}

private:
	// Measured: Desktop+ moves the cursor 120-140 ms after the laser lands on its panel.
	static constexpr int64_t kAimLeadMs = 200;
	static constexpr int64_t kAimHoldMs = 150;
	static constexpr double kRestDrop = 1.5; // metres

	std::atomic< bool > aiming_{ true };
	bool click_allowed_ = true;
	int64_t aim_since_ms_ = 0;
	int64_t last_engaged_ms_ = 0;
};
