// Flow dashboard helper: a SteamVR overlay app that opens the Desktop+ dashboard tab whenever
// the Flow (re)connects or a VR game exits, as long as no game is running, so the Flow shows
// the PC desktop without needing a controller. Games starting close the dashboard on their own.
// It waits for the Flow so the dashboard is placed in front of the head actually wearing it.
//
// While SteamVR runs it also captures the numeric keypad (the PC never sees those keys) and
// forwards the pressed buttons to driver_flowvr's keypad controller over localhost UDP, where
// they become SteamVR controller buttons on a laser that follows the head:
//   5 = trigger   0/Ins = grip   Enter = trackpad click   + / 8 = up   - / 2 = down
//   4 = left      6 = right      * = system (dashboard)   / = menu     other keypad keys = ignored
// NumLock and the keypad's Backspace (same key code as the main keyboard's) still pass through.
//
// Usage:
//   flow_dashboard_helper.exe            run (SteamVR auto-launches it after --install)
//   flow_dashboard_helper.exe --install  register with SteamVR and enable auto-launch
//   flow_dashboard_helper.exe --uninstall

#include <openvr.h>

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <string>
#include <thread>

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

	// Button bits; keep in sync with KeypadButton in flow_steamvr_driver/src/keyboard_mouse_controller.cpp.
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

	std::atomic< DWORD > g_hook_thread_id{ 0 };
	std::atomic< uint32_t > g_keypad_buttons{ 0 };
	SOCKET g_keypad_socket = INVALID_SOCKET;

	// Sends the current mask; also called periodically as a heartbeat (the driver releases all
	// buttons if it hears nothing for a while).
	void SendKeypadState()
	{
		if ( g_keypad_socket == INVALID_SOCKET )
		{
			return;
		}
		const uint32_t packet[ 2 ] = { kKeypadMagic, g_keypad_buttons.load() };
		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_port = htons( kKeypadPort );
		target.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		sendto( g_keypad_socket, reinterpret_cast< const char * >( packet ), sizeof( packet ), 0,
		        reinterpret_cast< const sockaddr * >( &target ), sizeof( target ) );
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

	Log( "started" );
	WSADATA wsa_data = {};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) == 0 )
	{
		g_keypad_socket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	}
	std::thread hook_thread( KeyboardHookThread );
	using Clock = std::chrono::steady_clock;
	bool open_pending = true; // SteamVR just started: open once the Flow is connected
	bool flow_connected = false;
	bool seated_zero_checked = false; // once per SteamVR session, after the head pose has settled
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
			else if ( event.eventType == vr::VREvent_SceneApplicationChanged && !GameRunning() )
			{
				Log( "VR game exited (or SteamVR Home is the scene)" );
				open_pending = true;
				open_eligible_at = {};
				scene_changed_at = Clock::now();
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
		SendKeypadState();
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}

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
