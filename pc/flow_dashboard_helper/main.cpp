// Flow dashboard helper: a SteamVR overlay app that opens the Desktop+ dashboard tab whenever
// the Flow (re)connects or a VR game exits, as long as no game is running, so the Flow shows
// the PC desktop without needing a controller, and closes the dashboard when a game takes over.
// It waits for the Flow so the dashboard is placed in front of the head actually wearing it.
//
// While SteamVR runs it also captures the numeric keypad (the PC never sees those keys) and
// forwards the pressed buttons to driver_flowvr's keypad controller over localhost UDP, where
// they become SteamVR controller buttons on a laser that follows the head:
//   5 = trigger   0/Ins = grip   Enter = trackpad click   + / 8 = up   - / 2 = down
//   4 = left      6 = right      * = system (dashboard)   / = menu     other keypad keys = ignored
// NumLock and the keypad's Backspace (same key code as the main keyboard's) still pass through.
// While the dashboard is open the keypad laser only shows while a key is held (see
// flow_pointer_gate.h in the driver), so a small head-locked reticle marks where it will land.
//
// Usage:
//   flow_dashboard_helper.exe            run (SteamVR auto-launches it after --install)
//   flow_dashboard_helper.exe --install  register with SteamVR and enable auto-launch
//   flow_dashboard_helper.exe --uninstall
//   flow_dashboard_helper.exe --idle-timeout <seconds>   set SteamVR's idle-to-standby timeout (SteamVR running)

#include <openvr.h>

#include "desktop_layer_streamer.h"

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <ctime>
#include <atomic>
#include <string>
#include <thread>
#include <algorithm>
#include <memory>

namespace
{
	constexpr const char *kAppKey = "flowvr.dashboard_helper";
	constexpr const char *kDesktopPlusDashboardKey = "elvissteinjr.DesktopPlusDashboard";
	// Desktop+ is auto-launched alongside us and may need a while to create its dashboard tab.
	constexpr auto kOpenRetryWindow = std::chrono::seconds( 60 );
	// Let the head pose settle after the Flow connects before placing the dashboard.
	constexpr auto kSettleAfterConnect = std::chrono::milliseconds( 1500 );
	// Scene change events arrive at the start of a transition (e.g. Home -> game, while Home is
	// still the scene). Wait this long and require a settled scene state before deciding.
	constexpr auto kSettleAfterSceneChange = std::chrono::milliseconds( 2000 );
	// Set by driver_flowvr on the HMD while the Flow receives the stream
	// (kProp_FlowStreamConnected_Bool in pc/flow_steamvr_driver/src/flow_pose_sync.h).
	constexpr auto kPropFlowStreamConnected = static_cast< vr::ETrackedDeviceProperty >( 10001 );


	// Reticle: on the gaze line where the keypad laser crosses it (kGazeConvergeDistance in
	// flow_steamvr_driver/src/keyboard_mouse_controller.cpp), i.e. where a keypad click lands.
	constexpr const char *kReticleKey = "flowvr.keypad_reticle";
	constexpr float kReticleDistance = 2.0f;
	constexpr float kReticleWidth = 0.03f; // metres, about 0.9 degrees at 2 m
	constexpr int kReticleSize = 64;

	std::string ExeDirectory()
	{
		char path[ MAX_PATH ] = {};
		GetModuleFileNameA( nullptr, path, MAX_PATH );
		std::string dir( path );
		return dir.substr( 0, dir.find_last_of( "\\/" ) );
	}

	void Log( const char *format, ... )
	{
		static FILE *file = nullptr;
		if ( file == nullptr )
		{
			file = std::fopen( ( ExeDirectory() + "\\flow_dashboard_helper.log" ).c_str(), "a" );
			if ( file == nullptr )
			{
				return;
			}
		}
		const std::time_t now = std::time( nullptr );
		char stamp[ 32 ];
		std::strftime( stamp, sizeof( stamp ), "%Y-%m-%d %H:%M:%S", std::localtime( &now ) );
		std::fprintf( file, "%s ", stamp );
		va_list args;
		va_start( args, format );
		std::vfprintf( file, format, args );
		va_end( args );
		std::fputc( '\n', file );
		std::fflush( file );
	}

}

// The shared NVENC encoder and the desktop layer streamer log through this.
void DriverLog( const char *format, ... )
{
	char line[ 512 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( line, sizeof( line ), format, args );
	va_end( args );
	Log( "%s", line );
}

namespace
{
	int Install( bool install )
	{
		const std::string manifest = ExeDirectory() + "\\flow_dashboard_helper.vrmanifest";
		vr::IVRApplications *apps = vr::VRApplications();
		if ( install )
		{
			const vr::EVRApplicationError add = apps->AddApplicationManifest( manifest.c_str(), false );
			const vr::EVRApplicationError autolaunch = apps->SetApplicationAutoLaunch( kAppKey, true );
			Log( "install manifest=%s add=%s autolaunch=%s", manifest.c_str(),
			     apps->GetApplicationsErrorNameFromEnum( add ), apps->GetApplicationsErrorNameFromEnum( autolaunch ) );
			return add == vr::VRApplicationError_None && autolaunch == vr::VRApplicationError_None ? 0 : 1;
		}
		apps->SetApplicationAutoLaunch( kAppKey, false );
		const vr::EVRApplicationError remove = apps->RemoveApplicationManifest( manifest.c_str() );
		Log( "uninstall manifest=%s remove=%s", manifest.c_str(), apps->GetApplicationsErrorNameFromEnum( remove ) );
		return remove == vr::VRApplicationError_None ? 0 : 1;
	}

	// ---- Numeric keypad -> driver_flowvr keypad controller ----------------------------------

	constexpr uint16_t kKeypadPort = 8003;
	constexpr uint32_t kKeypadMagic = 0x31504B46; // "FKP1"

	// Button bits; keep in sync with KeypadButton in flow_steamvr_driver/src/flow_shared_input.h.
	enum KeypadButton : uint32_t
	{
		KeypadButton_Trigger = 1u << 0,
		KeypadButton_Grip = 1u << 1,
		KeypadButton_TrackpadClick = 1u << 2,
		KeypadButton_Up = 1u << 3,
		KeypadButton_Down = 1u << 4,
		KeypadButton_Left = 1u << 5,
		KeypadButton_Right = 1u << 6,
		KeypadButton_System = 1u << 7,
		KeypadButton_Menu = 1u << 8,
	};

	// Status flags sent after the buttons; keep in sync with KeypadStatus in flow_shared_input.h.
	// The driver keeps its controllers' lasers off the overlays while the dashboard is open and
	// nothing is pressed, so Desktop+ does not drag the Windows cursor along with the head.
	constexpr uint32_t kKeypadStatusDashboardVisible = 1u << 0;
	// NumLock toggles hand tracking: while off the driver ignores the Flow's hands, so the keypad
	// stays the head-aimed pointer (its reticle shows; no reticle = hands are detected).
	constexpr uint32_t kKeypadStatusHandsDisabled = 1u << 1;
	std::atomic< bool > g_hands_disabled{ false };
	bool g_numlock_down = false; // hook thread only: ignore auto-repeat

	std::atomic< DWORD > g_hook_thread_id{ 0 };
	std::atomic< uint32_t > g_keypad_buttons{ 0 };
	std::atomic< uint32_t > g_keypad_status{ 0 };
	SOCKET g_keypad_socket = INVALID_SOCKET;

	// Sends the current mask; also called periodically as a heartbeat (the driver releases all
	// buttons if it hears nothing for a while).
	void SendKeypadState()
	{
		if ( g_keypad_socket == INVALID_SOCKET )
		{
			return;
		}
		const uint32_t packet[ 3 ] = { kKeypadMagic, g_keypad_buttons.load(), g_keypad_status.load() };
		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_port = htons( kKeypadPort );
		target.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		sendto( g_keypad_socket, reinterpret_cast< const char * >( packet ), sizeof( packet ), 0,
		        reinterpret_cast< const sockaddr * >( &target ), sizeof( target ) );
	}

	// Presses and releases the keypad controller's system button, which toggles the dashboard.
	// OpenVR has no call that closes the dashboard, so this is how we hide it.
	void TapSystemButton()
	{
		g_keypad_buttons.fetch_or( KeypadButton_System );
		SendKeypadState();
		std::this_thread::sleep_for( std::chrono::milliseconds( 150 ) );
		g_keypad_buttons.fetch_and( ~static_cast< uint32_t >( KeypadButton_System ) );
		SendKeypadState();
	}

	// Classifies a key: returns true if it comes from the numeric keypad (then it is swallowed)
	// and sets *button to its controller button (0 for keypad keys without a function).
	// NumLock changes the virtual key of most keypad keys; the extended flag tells keypad keys
	// apart from the dedicated arrow/Insert/Enter keys of the main keyboard.
	bool KeypadKey( const KBDLLHOOKSTRUCT &key, uint32_t *button )
	{
		const bool extended = ( key.flags & LLKHF_EXTENDED ) != 0;
		*button = 0;
		switch ( key.vkCode )
		{
		case VK_NUMPAD5: case VK_CLEAR: *button = KeypadButton_Trigger; return true;
		case VK_NUMPAD0: *button = KeypadButton_Grip; return true;
		case VK_ADD: case VK_NUMPAD8: *button = KeypadButton_Up; return true;
		case VK_SUBTRACT: case VK_NUMPAD2: *button = KeypadButton_Down; return true;
		case VK_NUMPAD4: *button = KeypadButton_Left; return true;
		case VK_NUMPAD6: *button = KeypadButton_Right; return true;
		case VK_MULTIPLY: *button = KeypadButton_System; return true;
		case VK_DIVIDE: *button = KeypadButton_Menu; return true;
		case VK_NUMPAD1: case VK_NUMPAD3: case VK_NUMPAD7: case VK_NUMPAD9: case VK_DECIMAL: return true;
		// NumLock off: these come from the keypad only when not extended.
		case VK_INSERT: *button = KeypadButton_Grip; return !extended;
		case VK_UP: *button = KeypadButton_Up; return !extended;
		case VK_DOWN: *button = KeypadButton_Down; return !extended;
		case VK_LEFT: *button = KeypadButton_Left; return !extended;
		case VK_RIGHT: *button = KeypadButton_Right; return !extended;
		case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DELETE: return !extended;
		case VK_RETURN: *button = KeypadButton_TrackpadClick; return extended;
		default: return false;
		}
	}

	LRESULT CALLBACK KeyboardHook( int code, WPARAM wparam, LPARAM lparam )
	{
		if ( code == HC_ACTION )
		{
			const auto *key = reinterpret_cast< const KBDLLHOOKSTRUCT * >( lparam );
			uint32_t button = 0;
			if ( !( key->flags & LLKHF_INJECTED ) && key->vkCode == VK_NUMLOCK )
			{
				const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
				if ( down && !g_numlock_down )
				{
					const bool disabled = !g_hands_disabled.load();
					g_hands_disabled = disabled;
					g_keypad_status = disabled ? ( g_keypad_status.load() | kKeypadStatusHandsDisabled )
					                           : ( g_keypad_status.load() & ~kKeypadStatusHandsDisabled );
					SendKeypadState();
					Log( disabled ? "NumLock: hand tracking off (keypad pointer only)" : "NumLock: hand tracking on" );
				}
				g_numlock_down = down;
				return 1; // NumLock is ours while SteamVR runs (the keypad works the same either way)
			}
			if ( !( key->flags & LLKHF_INJECTED ) && KeypadKey( *key, &button ) )
			{
				if ( button != 0 )
				{
					const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
					const uint32_t before = down ? g_keypad_buttons.fetch_or( button ) : g_keypad_buttons.fetch_and( ~button );
					if ( ( before & button ) != ( down ? button : 0u ) )
					{
						SendKeypadState(); // only on changes; auto-repeat is ignored
					}
				}
				return 1; // keypad keys never reach the PC while SteamVR runs
			}
		}
		return CallNextHookEx( nullptr, code, wparam, lparam );
	}

	// Low-level hooks are called on the installing thread, which must pump messages.
	void KeyboardHookThread()
	{
		HHOOK hook = SetWindowsHookExA( WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleA( nullptr ), 0 );
		Log( hook ? "keypad controller input active" : "keypad capture failed: SetWindowsHookEx error %lu", GetLastError() );
		g_hook_thread_id = GetCurrentThreadId();
		MSG msg;
		while ( GetMessageA( &msg, nullptr, 0, 0 ) > 0 )
		{
		}
		if ( hook )
		{
			UnhookWindowsHookEx( hook );
		}
	}

	bool FlowConnected()
	{
		vr::ETrackedPropertyError error = vr::TrackedProp_Success;
		const bool connected = vr::VRSystem()->GetBoolTrackedDeviceProperty(
			vr::k_unTrackedDeviceIndex_Hmd, kPropFlowStreamConnected, &error );
		return error == vr::TrackedProp_Success && connected;
	}

	// SteamVR launches SteamVR Home as a scene app at startup (when started from Steam) and
	// returns to it after games exit. It is our idle environment, not a game.
	constexpr const char *kSteamVRHomeAppKey = "openvr.tool.steamvr_environments";

	bool GameRunning()
	{
		const uint32_t pid = vr::VRApplications()->GetCurrentSceneProcessId();
		if ( pid == 0 )
		{
			return false;
		}
		char key[ vr::k_unMaxApplicationKeyLength ] = {};
		vr::VRApplications()->GetApplicationKeyByProcessId( pid, key, sizeof( key ) );
		return std::strcmp( key, kSteamVRHomeAppKey ) != 0;
	}

	bool SceneTransitioning()
	{
		const vr::EVRSceneApplicationState state = vr::VRApplications()->GetSceneApplicationState();
		return state == vr::EVRSceneApplicationState_Starting || state == vr::EVRSceneApplicationState_Quitting;
	}

	// Seated apps (Unity's default, e.g. KoikatuVR and the SteamVR Media Player) get invalid head
	// poses until the seated zero pose of the Flow's tracking universe has been set, and SteamVR
	// then fades them to its trackingLossColor (a flat grey). Set it from the current head pose
	// when it is missing.
	void EnsureSeatedZeroPose()
	{
		vr::TrackedDevicePose_t pose{};
		vr::VRSystem()->GetDeviceToAbsoluteTrackingPose( vr::TrackingUniverseSeated, 0.f, &pose, 1 );
		if ( pose.bPoseIsValid )
		{
			return;
		}
		vr::VRChaperone()->ResetZeroPose( vr::TrackingUniverseSeated );
		Log( "seated zero pose was not set: reset it to the current head pose" );
	}

	// White ring with a dark outline and a centre dot, readable on light and dark desktops.
	std::vector< uint8_t > ReticlePixels()
	{
		std::vector< uint8_t > pixels( kReticleSize * kReticleSize * 4, 0 );
		const float centre = ( kReticleSize - 1 ) * 0.5f;
		for ( int y = 0; y < kReticleSize; ++y )
		{
			for ( int x = 0; x < kReticleSize; ++x )
			{
				const float r = std::hypot( x - centre, y - centre );
				uint8_t value = 0, alpha = 0;
				if ( ( r >= 20.0f && r <= 25.0f ) || r <= 3.0f )
				{
					value = 255, alpha = 255;
				}
				else if ( ( r >= 17.0f && r <= 28.0f ) || r <= 5.5f )
				{
					value = 0, alpha = 200;
				}
				uint8_t *p = &pixels[ ( y * kReticleSize + x ) * 4 ];
				p[ 0 ] = p[ 1 ] = p[ 2 ] = value;
				p[ 3 ] = alpha;
			}
		}
		return pixels;
	}

	vr::VROverlayHandle_t CreateReticle()
	{
		vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
		if ( vr::VROverlay()->CreateOverlay( kReticleKey, "Flow keypad reticle", &handle ) != vr::VROverlayError_None )
		{
			Log( "reticle overlay could not be created" );
			return vr::k_ulOverlayHandleInvalid;
		}
		std::vector< uint8_t > pixels = ReticlePixels();
		vr::VROverlay()->SetOverlayRaw( handle, pixels.data(), kReticleSize, kReticleSize, 4 );
		vr::VROverlay()->SetOverlayWidthInMeters( handle, kReticleWidth );
		vr::VROverlay()->SetOverlaySortOrder( handle, 0xFFFFFFFF );
		vr::HmdMatrix34_t transform = {};
		transform.m[ 0 ][ 0 ] = transform.m[ 1 ][ 1 ] = transform.m[ 2 ][ 2 ] = 1.0f;
		transform.m[ 2 ][ 3 ] = -kReticleDistance;
		vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative( handle, vr::k_unTrackedDeviceIndex_Hmd, &transform );
		return handle;
	}

	// ---- Desktop+ panel -> driver_flowvr -> Flow desktop layer ---------------------------------
	// The Flow shows the PC desktop as a compositor layer (sharper than the streamed SteamVR
	// picture) exactly where Desktop+ draws its panel, so Desktop+ keeps handling the laser,
	// clicks and its tools. Sent every loop: magic, flags (bit 0 = show, bit 1 = the head-aimed
	// keypad pointer is active: draw its reticle on the layer, which hides ours), the panel's 3x4
	// transform in the driver's raw tracking space (row-major, includes Desktop+'s scale), width,
	// and the overlay's texture bounds (which part of Desktop+'s all-monitors texture it shows).
	constexpr uint32_t kDesktopPanelMagic = 0x31504446; // "FDP1"
	// Desktop+ overlays are "elvissteinjr.DesktopPlus<n>" (n = its overlay list: Desktop 1,
	// Desktop 2, windows...). The dashboard shows one at a time (its 1/2 buttons switch).
	constexpr const char *kDesktopPlusOverlayPrefix = "elvissteinjr.DesktopPlus";
	constexpr int kDesktopPlusMaxOverlays = 16;

	// The Desktop+ overlay currently shown, or k_ulOverlayHandleInvalid.
	vr::VROverlayHandle_t FindShownDesktopPlusPanel()
	{
		for ( int i = 0; i < kDesktopPlusMaxOverlays; ++i )
		{
			const std::string key = kDesktopPlusOverlayPrefix + std::to_string( i );
			vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
			if ( vr::VROverlay()->FindOverlay( key.c_str(), &handle ) != vr::VROverlayError_None )
			{
				break;
			}
			if ( vr::VROverlay()->IsOverlayVisible( handle ) )
			{
				return handle;
			}
		}
		return vr::k_ulOverlayHandleInvalid;
	}

	struct DesktopPanelPacket
	{
		uint32_t magic;
		uint32_t flags;
		float transform[ 12 ];
		float width;
		float bounds[ 4 ]; // uMin, uMax, vMin, vMax
	};

	vr::HmdMatrix34_t RigidInverse( const vr::HmdMatrix34_t &m )
	{
		vr::HmdMatrix34_t r = {};
		for ( int i = 0; i < 3; ++i )
		{
			for ( int j = 0; j < 3; ++j )
			{
				r.m[ i ][ j ] = m.m[ j ][ i ];
			}
			r.m[ i ][ 3 ] = -( m.m[ 0 ][ i ] * m.m[ 0 ][ 3 ] + m.m[ 1 ][ i ] * m.m[ 1 ][ 3 ] + m.m[ 2 ][ i ] * m.m[ 2 ][ 3 ] );
		}
		return r;
	}

	vr::HmdMatrix34_t Multiply( const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b )
	{
		vr::HmdMatrix34_t r = {};
		for ( int i = 0; i < 3; ++i )
		{
			for ( int j = 0; j < 4; ++j )
			{
				r.m[ i ][ j ] = a.m[ i ][ 0 ] * b.m[ 0 ][ j ] + a.m[ i ][ 1 ] * b.m[ 1 ][ j ] + a.m[ i ][ 2 ] * b.m[ 2 ][ j ] +
				                ( j == 3 ? a.m[ i ][ 3 ] : 0.f );
			}
		}
		return r;
	}

	// Sends where the shown Desktop+ panel is; returns it (k_ulOverlayHandleInvalid if none shown).
	vr::VROverlayHandle_t SendDesktopPanelState( bool dashboard_visible, bool keypad_pointer )
	{
		if ( g_keypad_socket == INVALID_SOCKET )
		{
			return vr::k_ulOverlayHandleInvalid;
		}
		DesktopPanelPacket packet = { kDesktopPanelMagic, 0, {}, 0.f, {} };
		const vr::VROverlayHandle_t handle = dashboard_visible ? FindShownDesktopPlusPanel() : vr::k_ulOverlayHandleInvalid;
		if ( handle != vr::k_ulOverlayHandleInvalid )
		{
			vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
			vr::HmdMatrix34_t transform = {};
			float width = 0.f;
			if ( vr::VROverlay()->GetOverlayTransformAbsolute( handle, &origin, &transform ) == vr::VROverlayError_None &&
			     vr::VROverlay()->GetOverlayWidthInMeters( handle, &width ) == vr::VROverlayError_None &&
			     origin != vr::TrackingUniverseSeated )
			{
				if ( origin == vr::TrackingUniverseStanding )
				{
					const vr::HmdMatrix34_t raw_from_standing =
						RigidInverse( vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose() );
					transform = Multiply( raw_from_standing, transform );
				}
				packet.flags = 1u | ( keypad_pointer ? 2u : 0u );
				std::memcpy( packet.transform, transform.m, sizeof( packet.transform ) );
				packet.width = width;
				vr::VRTextureBounds_t bounds = {};
				if ( vr::VROverlay()->GetOverlayTextureBounds( handle, &bounds ) == vr::VROverlayError_None )
				{
					packet.bounds[ 0 ] = bounds.uMin;
					packet.bounds[ 1 ] = bounds.uMax;
					packet.bounds[ 2 ] = bounds.vMin;
					packet.bounds[ 3 ] = bounds.vMax;
				}
			}
		}
		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_port = htons( kKeypadPort );
		target.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		sendto( g_keypad_socket, reinterpret_cast< const char * >( &packet ), sizeof( packet ), 0,
		        reinterpret_cast< const sockaddr * >( &target ), sizeof( target ) );
		return ( packet.flags & 1u ) != 0 ? handle : vr::k_ulOverlayHandleInvalid;
	}

	// While the Flow covers the Desktop+ panel with its sharp layer, show the panel itself black:
	// the streamed picture lags the head a little, so its blurry copy would otherwise peek out
	// around the layer on head movement. Colour only: making the panel transparent (alpha) stops
	// Desktop+ taking laser input, and it does not recover until Desktop+ restarts.
	vr::VROverlayHandle_t g_desktop_panel_blacked = vr::k_ulOverlayHandleInvalid;

	// Blacks out `handle` (the panel the Flow covers), or none; restores the one blacked before.
	void UpdateDesktopPanelTint( vr::VROverlayHandle_t handle )
	{
		if ( g_desktop_panel_blacked != vr::k_ulOverlayHandleInvalid && g_desktop_panel_blacked != handle )
		{
			vr::VROverlay()->SetOverlayColor( g_desktop_panel_blacked, 1.f, 1.f, 1.f );
			g_desktop_panel_blacked = vr::k_ulOverlayHandleInvalid;
			Log( "Flow desktop layer off this panel: Desktop+ panel restored" );
		}
		if ( handle != vr::k_ulOverlayHandleInvalid )
		{
			float r = 1.f, g = 1.f, b = 1.f;
			vr::VROverlay()->GetOverlayColor( handle, &r, &g, &b );
			if ( r + g + b > 0.001f ) // set by us, or reset by Desktop+
			{
				vr::VROverlay()->SetOverlayColor( handle, 0.f, 0.f, 0.f );
				if ( g_desktop_panel_blacked != handle )
				{
					Log( "Flow desktop layer active: Desktop+ panel blacked out" );
				}
			}
			g_desktop_panel_blacked = handle;
		}
	}

	// Returns true once the Desktop+ tab has been shown (or there is no reason to show it anymore).
	bool TryOpenDesktopTab()
	{
		if ( GameRunning() )
		{
			return true; // a game took over; leave the dashboard alone
		}
		if ( SceneTransitioning() )
		{
			return false; // an app is starting/quitting; decide once it settles
		}
		vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
		if ( vr::VROverlay()->FindOverlay( kDesktopPlusDashboardKey, &handle ) != vr::VROverlayError_None )
		{
			return false; // Desktop+ not up yet
		}
		vr::VROverlay()->ShowDashboard( kDesktopPlusDashboardKey );
		Log( "no VR game running: opened Desktop+ dashboard tab" );
		return true;
	}
}

int main( int argc, char **argv )
{
	const std::string arg = argc > 1 ? argv[ 1 ] : "";

	// One instance at a time (SteamVR auto-launch plus a manual start would otherwise race).
	if ( arg.empty() )
	{
		CreateMutexA( nullptr, TRUE, "Local\\FlowDashboardHelper" );
		if ( GetLastError() == ERROR_ALREADY_EXISTS )
		{
			return 0;
		}
	}

	vr::EVRInitError init_error = vr::VRInitError_None;
	vr::VR_Init( &init_error, vr::VRApplication_Overlay );
	if ( init_error != vr::VRInitError_None )
	{
		Log( "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription( init_error ) );
		return 1;
	}

	if ( arg == "--install" || arg == "--uninstall" )
	{
		const int result = Install( arg == "--install" );
		vr::VR_Shutdown();
		return result;
	}

	// Changes SteamVR's idle-to-standby timeout while SteamVR runs (it rewrites
	// steamvr.vrsettings on exit, so editing the file then would be lost). Used by
	// scripts\dev-awake.ps1.
	if ( arg == "--idle-timeout" )
	{
		const float seconds = argc > 2 ? static_cast< float >( std::atof( argv[ 2 ] ) ) : 0.f;
		vr::EVRSettingsError error = vr::VRSettingsError_None;
		if ( seconds > 0.f )
		{
			vr::VRSettings()->SetFloat( vr::k_pch_Power_Section, vr::k_pch_Power_TurnOffScreensTimeout_Float, seconds, &error );
		}
		const int result = seconds > 0.f && error == vr::VRSettingsError_None ? 0 : 1;
		Log( result == 0 ? "power.turnOffScreensTimeout = %.0f s" : "failed to set power.turnOffScreensTimeout (%.0f s)", seconds );
		vr::VR_Shutdown();
		return result;
	}

	Log( "started" );
	WSADATA wsa_data = {};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) == 0 )
	{
		g_keypad_socket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	}
	std::thread hook_thread( KeyboardHookThread );
	// Desktop+'s panel picture, streamed to the Flow's sharp desktop layer (TCP 8005).
	std::unique_ptr< DesktopLayerStreamer > desktop_layer;
	{
		vr::EVRSettingsError error = vr::VRSettingsError_None;
		const bool enabled = vr::VRSettings()->GetBool( "driver_flowvr", "enable_desktop_layer", &error );
		if ( error != vr::VRSettingsError_None || enabled )
		{
			int32_t mbps = vr::VRSettings()->GetInt32( "driver_flowvr", "desktop_bitrate_mbps", &error );
			mbps = error == vr::VRSettingsError_None ? ( std::max )( mbps, 5 ) : 30;
			int32_t fps = vr::VRSettings()->GetInt32( "driver_flowvr", "desktop_fps", &error );
			fps = error == vr::VRSettingsError_None ? std::clamp( fps, 10, 75 ) : 60;
			desktop_layer = std::make_unique< DesktopLayerStreamer >( static_cast< uint32_t >( mbps ) * 1000000u, static_cast< uint32_t >( fps ) );
		}
	}
	const vr::VROverlayHandle_t reticle = CreateReticle();
	bool reticle_shown = false;
	using Clock = std::chrono::steady_clock;
	bool open_pending = true; // SteamVR just started: open once the Flow is connected
	bool flow_connected = false;
	bool seated_zero_checked = false; // once per SteamVR session, after the head pose has settled
	bool close_check_pending = false; // scene changed: close the dashboard if a game took over
	Clock::time_point connected_at{};
	Clock::time_point open_eligible_at{}; // when the pending open first became possible
	Clock::time_point scene_changed_at{};
	bool running = true;
	while ( running )
	{
		vr::VREvent_t event{};
		while ( vr::VRSystem()->PollNextEvent( &event, sizeof( event ) ) )
		{
			if ( event.eventType == vr::VREvent_Quit )
			{
				vr::VRSystem()->AcknowledgeQuit_Exiting();
				running = false;
			}
			else if ( event.eventType == vr::VREvent_SceneApplicationChanged )
			{
				scene_changed_at = Clock::now();
				close_check_pending = true;
				if ( !GameRunning() )
				{
					Log( "VR game exited (or SteamVR Home is the scene)" );
					open_pending = true;
					open_eligible_at = {};
				}
			}
		}

		const auto now = Clock::now();
		const bool connected = FlowConnected();
		if ( connected != flow_connected )
		{
			flow_connected = connected;
			Log( connected ? "Flow connected" : "Flow disconnected" );
			if ( connected )
			{
				connected_at = now;
				open_pending = true; // e.g. headset put back on
				open_eligible_at = {};
			}
		}

		// A game does not take over from an open dashboard: the old SteamVR Unity plugin, for one,
		// pauses and renders at half resolution while it lacks input focus. Close the dashboard
		// (usually the Desktop+ tab we opened) once the game has settled as the scene.
		if ( close_check_pending && running && now - scene_changed_at >= kSettleAfterSceneChange && !SceneTransitioning() )
		{
			close_check_pending = false;
			if ( GameRunning() && vr::VROverlay()->IsDashboardVisible() )
			{
				TapSystemButton();
				Log( "VR game started: closed the dashboard" );
			}
		}

		if ( !seated_zero_checked && running && flow_connected && now - connected_at >= kSettleAfterConnect )
		{
			EnsureSeatedZeroPose();
			seated_zero_checked = true;
		}

		if ( open_pending && running && flow_connected && now - connected_at >= kSettleAfterConnect &&
		     now - scene_changed_at >= kSettleAfterSceneChange )
		{
			if ( open_eligible_at == Clock::time_point{} )
			{
				open_eligible_at = now;
			}
			if ( TryOpenDesktopTab() )
			{
				open_pending = false;
			}
			else if ( now - open_eligible_at > kOpenRetryWindow )
			{
				Log( "Desktop+ dashboard tab not found within %lld s; giving up until the Flow reconnects or a game exits",
				     static_cast< long long >( kOpenRetryWindow.count() ) );
				open_pending = false;
			}
		}
		const bool dashboard_visible = vr::VROverlay()->IsDashboardVisible();
		g_keypad_status = ( dashboard_visible ? kKeypadStatusDashboardVisible : 0u ) |
		                  ( g_hands_disabled ? kKeypadStatusHandsDisabled : 0u );
		// Reticle = hand tracking is off (NumLock); without it the hands drive the pointers.
		const bool show_reticle = dashboard_visible && g_hands_disabled;
		const vr::VROverlayHandle_t panel = SendDesktopPanelState( dashboard_visible, show_reticle );
		if ( desktop_layer )
		{
			desktop_layer->SetPanel( panel );
		}
		UpdateDesktopPanelTint( desktop_layer && desktop_layer->Streaming() ? panel : vr::k_ulOverlayHandleInvalid );
		if ( reticle != vr::k_ulOverlayHandleInvalid && show_reticle != reticle_shown )
		{
			reticle_shown = show_reticle;
			if ( show_reticle )
			{
				vr::VROverlay()->ShowOverlay( reticle );
			}
			else
			{
				vr::VROverlay()->HideOverlay( reticle );
			}
		}
		SendKeypadState();
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}

	UpdateDesktopPanelTint( vr::k_ulOverlayHandleInvalid );
	desktop_layer.reset();
	Log( "SteamVR quit" );
	while ( g_hook_thread_id == 0 )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
	}
	PostThreadMessageA( g_hook_thread_id, WM_QUIT, 0, 0 );
	hook_thread.join();
	g_keypad_buttons = 0;
	SendKeypadState(); // release everything right away
	closesocket( g_keypad_socket );
	WSACleanup();
	vr::VR_Shutdown();
	return 0;
}
