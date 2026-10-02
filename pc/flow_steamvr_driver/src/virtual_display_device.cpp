//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "virtual_display_device.h"

#include "driverlog.h"
#include "flow_nvenc_encoder.h"
#include "flow_pose_sync.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#endif

namespace
{
	constexpr float kDisplayFrequencyHz = 75.0f;
	constexpr auto kFrameDuration = std::chrono::duration< double >( 1.0 / kDisplayFrequencyHz );
#ifdef _WIN32
	constexpr uint32_t kStreamPreviewFps = 75;
	constexpr uint16_t kStreamPreviewPort = 8001;
	constexpr uint16_t kStreamDiscoveryPort = 8002;
	constexpr auto kStreamDiscoveryInterval = std::chrono::seconds( 1 );
	constexpr auto kStreamReconnectInterval = std::chrono::seconds( 2 );
	constexpr auto kStreamFrameInterval = std::chrono::duration< double >( 1.0 / kStreamPreviewFps );
	constexpr FlowSocketHandle kInvalidFlowSocket = ~static_cast< FlowSocketHandle >( 0 );
	std::atomic< FlowVirtualDisplayDevice * > g_stream_owner{ nullptr };
#endif

	uint64_t GetPrimaryAdapterLuid()
	{
#ifdef _WIN32
		IDXGIFactory1 *factory = nullptr;
		if ( FAILED( CreateDXGIFactory1( __uuidof( IDXGIFactory1 ), reinterpret_cast< void ** >( &factory ) ) ) )
		{
			return 0;
		}

		IDXGIAdapter1 *adapter = nullptr;
		if ( FAILED( factory->EnumAdapters1( 0, &adapter ) ) )
		{
			factory->Release();
			return 0;
		}

		DXGI_ADAPTER_DESC1 desc{};
		const HRESULT hr = adapter->GetDesc1( &desc );
		adapter->Release();
		factory->Release();

		if ( FAILED( hr ) )
		{
			return 0;
		}

		uint64_t luid = 0;
		static_assert( sizeof( luid ) == sizeof( desc.AdapterLuid ), "LUID size mismatch" );
		std::memcpy( &luid, &desc.AdapterLuid, sizeof( luid ) );
		return luid;
#else
		return 0;
#endif
	}

	// <driver root>\logs\<file>, where <driver root> is the folder registered with SteamVR
	// (driver_flowvr.dll lives in <driver root>\bin\win64). Created on first use.
	std::string DriverLogPath( const char *file )
	{
		static const std::string logs_dir = []()
		{
			HMODULE module = nullptr;
			GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                    reinterpret_cast< LPCSTR >( &DriverLogPath ), &module );
			char dll_path[ MAX_PATH ] = {};
			GetModuleFileNameA( module, dll_path, MAX_PATH );
			std::string dir( dll_path );
			for ( int up = 0; up < 3; ++up ) // strip \driver_flowvr.dll, \win64, \bin
			{
				dir = dir.substr( 0, dir.find_last_of( "\\/" ) );
			}
			dir += "\\logs";
			CreateDirectoryA( dir.c_str(), nullptr );
			return dir;
		}();
		return logs_dir + "\\" + file;
	}

	void TraceVirtualDisplayCall( const char *line )
	{
		static std::mutex trace_mutex;
		static const std::string trace_path = DriverLogPath( "flow_virtual_display_trace.log" );
		std::lock_guard< std::mutex > lock( trace_mutex );
		std::ofstream trace( trace_path, std::ios::app );
		trace << line << '\n';
	}

#ifdef _WIN32
	void SafeReleaseTextureMap( std::unordered_map< uint64_t, ID3D11Texture2D * > &textures )
	{
		for ( auto &entry : textures )
		{
			if ( entry.second != nullptr )
			{
				entry.second->Release();
			}
		}
		textures.clear();
	}

	uint32_t HostToBigEndian32( uint32_t value )
	{
		return htonl( value );
	}

	uint64_t HostToBigEndian64( uint64_t value )
	{
		const uint32_t high = htonl( static_cast< uint32_t >( value >> 32 ) );
		const uint32_t low = htonl( static_cast< uint32_t >( value & 0xffffffffu ) );
		return ( static_cast< uint64_t >( low ) << 32 ) | high;
	}

	int64_t HostToBigEndianSigned64( int64_t value )
	{
		uint64_t unsigned_value = 0;
		std::memcpy( &unsigned_value, &value, sizeof( unsigned_value ) );
		unsigned_value = HostToBigEndian64( unsigned_value );
		int64_t out = 0;
		std::memcpy( &out, &unsigned_value, sizeof( out ) );
		return out;
	}

	uint64_t EpochMilliseconds()
	{
		return static_cast< uint64_t >(
			std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::system_clock::now().time_since_epoch() )
				.count() );
	}

	size_t StartCodeLength( const std::vector< uint8_t > &data, size_t offset )
	{
		if ( offset + 3 <= data.size() && data[ offset ] == 0 && data[ offset + 1 ] == 0 && data[ offset + 2 ] == 1 )
		{
			return 3;
		}
		if ( offset + 4 <= data.size() && data[ offset ] == 0 && data[ offset + 1 ] == 0 && data[ offset + 2 ] == 0 && data[ offset + 3 ] == 1 )
		{
			return 4;
		}
		return 0;
	}

	size_t FindStartCode( const std::vector< uint8_t > &data, size_t start )
	{
		for ( size_t i = start; i + 3 < data.size(); ++i )
		{
			if ( data[ i ] == 0 && data[ i + 1 ] == 0 )
			{
				if ( data[ i + 2 ] == 1 )
				{
					return i;
				}
				if ( i + 3 < data.size() && data[ i + 2 ] == 0 && data[ i + 3 ] == 1 )
				{
					return i;
				}
			}
		}
		return std::string::npos;
	}

	std::vector< std::pair< size_t, size_t > > AnnexBNals( const std::vector< uint8_t > &packet )
	{
		std::vector< std::pair< size_t, size_t > > nals;
		size_t start = FindStartCode( packet, 0 );
		while ( start != std::string::npos )
		{
			const size_t next_search = start + StartCodeLength( packet, start );
			const size_t next = FindStartCode( packet, next_search );
			const size_t end = ( next == std::string::npos ) ? packet.size() : next;
			if ( end > start )
			{
				nals.emplace_back( start, end );
			}
			start = next;
		}
		return nals;
	}

	int NalType( const std::vector< uint8_t > &packet, size_t start )
	{
		const size_t code_len = StartCodeLength( packet, start );
		const size_t nal_header = start + code_len;
		if ( code_len == 0 || nal_header >= packet.size() )
		{
			return -1;
		}
		return packet[ nal_header ] & 0x1f;
	}

	void SafeReleaseD3DBlob( ID3DBlob *blob )
	{
		if ( blob != nullptr )
		{
			blob->Release();
		}
	}
#endif
}

FlowVirtualDisplayDevice::FlowVirtualDisplayDevice()
	: serial_number_( "VIVEFLOW-DISPLAYREDIRECT-001" ),
	  graphics_adapter_luid_( GetPrimaryAdapterLuid() ),
	  start_time_( std::chrono::steady_clock::now() ),
	  last_vsync_( start_time_ )
{
#ifdef _WIN32
	last_stream_connect_attempt_ = start_time_ - kStreamReconnectInterval;
	next_stream_frame_ = start_time_;

	// H.264 needs even dimensions; fall back to the known-good size on bad settings.
	const int32_t stream_width = vr::VRSettings()->GetInt32( "flowvr_display", "stream_width" );
	const int32_t stream_height = vr::VRSettings()->GetInt32( "flowvr_display", "stream_height" );
	if ( stream_width >= 256 && stream_height >= 128 && stream_width % 2 == 0 && stream_height % 2 == 0 )
	{
		stream_width_ = static_cast< uint32_t >( stream_width );
		stream_height_ = static_cast< uint32_t >( stream_height );
	}
	DriverLog( "Flow virtual display stream size %ux%u (side-by-side stereo)", stream_width_, stream_height_ );

	// The Flow's AVC decoder accepts up to 120 Mbit/s.
	const int32_t bitrate_mbps = vr::VRSettings()->GetInt32( "flowvr_display", "stream_bitrate_mbps" );
	if ( bitrate_mbps >= 10 && bitrate_mbps <= 120 )
	{
		stream_bitrate_ = static_cast< uint32_t >( bitrate_mbps ) * 1000000u;
	}
	const int32_t preset = vr::VRSettings()->GetInt32( "flowvr_display", "nvenc_preset" );
	if ( preset >= 1 && preset <= 7 )
	{
		nvenc_preset_ = static_cast< uint32_t >( preset );
	}
	DriverLog( "Flow virtual display encoder %u Mbit/s, NVENC preset P%u", stream_bitrate_ / 1000000u, nvenc_preset_ );
#endif
	DriverLog( "Flow virtual display serial number: %s", serial_number_.c_str() );
	DriverLog( "Flow virtual display graphics adapter LUID: 0x%llx",
	           static_cast< unsigned long long >( graphics_adapter_luid_ ) );
}

FlowVirtualDisplayDevice::~FlowVirtualDisplayDevice()
{
	ReleaseD3DResources();
}

vr::EVRInitError FlowVirtualDisplayDevice::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;
	is_active_ = true;
	start_time_ = std::chrono::steady_clock::now();
	last_vsync_ = start_time_;
	present_count_ = 0;
	vsync_counter_ = 0;
#ifdef _WIN32
	nvenc_failed_ = false;
#endif

	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, "VIVE Flow Virtual Display Redirect" );
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_NeverTracked_Bool, true );
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, kDisplayFrequencyHz );
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f );
	vr::VRProperties()->SetUint64Property( container, vr::Prop_GraphicsAdapterLuid_Uint64, graphics_adapter_luid_ );

	DriverLog( "Flow virtual display activated as DisplayRedirect device index %u", device_index_ );
	return vr::VRInitError_None;
}

void FlowVirtualDisplayDevice::Deactivate()
{
	if ( is_active_.exchange( false ) )
	{
		DriverLog( "Flow virtual display deactivated after %llu Present calls",
		           static_cast< unsigned long long >( present_count_.load() ) );
	}
	ReleaseD3DResources();
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void FlowVirtualDisplayDevice::EnterStandby()
{
	DriverLog( "Flow virtual display entered standby" );
}

void *FlowVirtualDisplayDevice::GetComponent( const char *pchComponentNameAndVersion )
{
	if ( std::strcmp( pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version ) == 0 )
	{
		DriverLog( "Flow virtual display component requested: %s", pchComponentNameAndVersion );
		return static_cast< vr::IVRVirtualDisplay * >( this );
	}

	return nullptr;
}

void FlowVirtualDisplayDevice::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize >= 1 )
	{
		pchResponseBuffer[ 0 ] = 0;
	}
}

vr::DriverPose_t FlowVirtualDisplayDevice::GetPose()
{
	vr::DriverPose_t pose = { 0 };
	pose.qWorldFromDriverRotation.w = 1.f;
	pose.qDriverFromHeadRotation.w = 1.f;
	pose.qRotation.w = 1.f;
	pose.deviceIsConnected = true;
	pose.poseIsValid = true;
	pose.result = vr::TrackingResult_Running_OK;
	return pose;
}

void FlowVirtualDisplayDevice::Present( const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize )
{
	if ( pPresentInfo == nullptr || unPresentInfoSize < sizeof( vr::PresentInfo_t ) )
	{
		DriverLog( "Flow virtual display Present received invalid payload size=%u", unPresentInfoSize );
		return;
	}

	const uint64_t count = ++present_count_;
	ProbeSharedTexture( *pPresentInfo, count );
	if ( count == 1 || ( count % 300 ) == 0 )
	{
		DriverLog( "Flow virtual display Present count=%llu frameId=%llu texture=0x%llx vsync=%d vsyncTime=%.6f size=%u",
		           static_cast< unsigned long long >( count ),
		           static_cast< unsigned long long >( pPresentInfo->nFrameId ),
		           static_cast< unsigned long long >( pPresentInfo->backbufferTextureHandle ),
		           static_cast< int >( pPresentInfo->vsync ),
		           pPresentInfo->flVSyncTimeInSeconds,
		           unPresentInfoSize );
		char trace_line[ 256 ];
		std::snprintf( trace_line, sizeof( trace_line ), "Present count=%llu frameId=%llu texture=0x%llx size=%u",
		               static_cast< unsigned long long >( count ),
		               static_cast< unsigned long long >( pPresentInfo->nFrameId ),
		               static_cast< unsigned long long >( pPresentInfo->backbufferTextureHandle ),
		               unPresentInfoSize );
		TraceVirtualDisplayCall( trace_line );
	}
}

bool FlowVirtualDisplayDevice::InitializeD3DResources()
{
#ifdef _WIN32
	if ( d3d_device_ != nullptr && d3d_context_ != nullptr )
	{
		return true;
	}

	if ( dxgi_factory_ == nullptr )
	{
		if ( FAILED( CreateDXGIFactory1( __uuidof( IDXGIFactory1 ), reinterpret_cast< void ** >( &dxgi_factory_ ) ) ) )
		{
			DriverLog( "Flow virtual display D3D: CreateDXGIFactory1 failed" );
			TraceVirtualDisplayCall( "D3D CreateDXGIFactory1 failed" );
			return false;
		}
	}

	IDXGIAdapter1 *adapter = nullptr;
	if ( FAILED( dxgi_factory_->EnumAdapters1( 0, &adapter ) ) )
	{
		DriverLog( "Flow virtual display D3D: EnumAdapters1(0) failed" );
		TraceVirtualDisplayCall( "D3D EnumAdapters1(0) failed" );
		return false;
	}

	D3D_FEATURE_LEVEL feature_level{};
	const HRESULT hr = D3D11CreateDevice( adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
	                                      D3D11_SDK_VERSION, &d3d_device_, &feature_level, &d3d_context_ );
	adapter->Release();

	if ( FAILED( hr ) )
	{
		char trace_line[ 128 ];
		std::snprintf( trace_line, sizeof( trace_line ), "D3D11CreateDevice failed hr=0x%08x", static_cast< unsigned int >( hr ) );
		DriverLog( "Flow virtual display D3D: %s", trace_line );
		TraceVirtualDisplayCall( trace_line );
		return false;
	}

	// The encode thread drives NVENC on this device while Present renders on it. Without
	// multithread protection the two raced inside the immediate context and hung Present
	// while it held the compositor's keyed mutex (SteamVR froze).
	ID3D11Multithread *multithread = nullptr;
	if ( SUCCEEDED( d3d_context_->QueryInterface( __uuidof( ID3D11Multithread ), reinterpret_cast< void ** >( &multithread ) ) ) )
	{
		multithread->SetMultithreadProtected( TRUE );
		multithread->Release();
	}

	char trace_line[ 128 ];
	std::snprintf( trace_line, sizeof( trace_line ), "D3D initialized featureLevel=0x%x", static_cast< unsigned int >( feature_level ) );
	DriverLog( "Flow virtual display D3D: %s", trace_line );
	TraceVirtualDisplayCall( trace_line );
	return true;
#else
	return false;
#endif
}

ID3D11Texture2D *FlowVirtualDisplayDevice::OpenSharedTexture( vr::SharedTextureHandle_t texture_handle )
{
#ifdef _WIN32
	if ( texture_handle == 0 )
	{
		return nullptr;
	}

	const uint64_t key = static_cast< uint64_t >( texture_handle );
	const auto found = shared_textures_.find( key );
	if ( found != shared_textures_.end() )
	{
		return found->second;
	}

	ID3D11Texture2D *texture = nullptr;
	const HRESULT hr = d3d_device_->OpenSharedResource( reinterpret_cast< HANDLE >( texture_handle ),
	                                                    __uuidof( ID3D11Texture2D ),
	                                                    reinterpret_cast< void ** >( &texture ) );
	if ( FAILED( hr ) )
	{
		char trace_line[ 160 ];
		std::snprintf( trace_line, sizeof( trace_line ), "OpenSharedResource failed handle=0x%llx hr=0x%08x",
		               static_cast< unsigned long long >( texture_handle ),
		               static_cast< unsigned int >( hr ) );
		DriverLog( "Flow virtual display D3D: %s", trace_line );
		TraceVirtualDisplayCall( trace_line );
		return nullptr;
	}

	shared_textures_[ key ] = texture;
	return texture;
#else
	return nullptr;
#endif
}

void FlowVirtualDisplayDevice::ProbeSharedTexture( const vr::PresentInfo_t &present_info, uint64_t present_count )
{
#ifdef _WIN32
	std::lock_guard< std::mutex > lock( d3d_mutex_ );

	if ( !InitializeD3DResources() )
	{
		return;
	}

	ID3D11Texture2D *texture = OpenSharedTexture( present_info.backbufferTextureHandle );
	if ( texture == nullptr )
	{
		return;
	}

	FlowVirtualDisplayDevice *expected_owner = nullptr;
	if ( !owns_stream_ )
	{
		if ( g_stream_owner.compare_exchange_strong( expected_owner, this ) )
		{
			owns_stream_ = true;
			TraceVirtualDisplayCall( "FLOWH264 direct stream owner acquired" );
			StartEncodeThread();
		}
		else if ( expected_owner != this )
		{
			return;
		}
	}

	IDXGIKeyedMutex *keyed_mutex = nullptr;
	const bool has_keyed_mutex = SUCCEEDED( texture->QueryInterface( __uuidof( IDXGIKeyedMutex ),
	                                                                 reinterpret_cast< void ** >( &keyed_mutex ) ) );
	if ( has_keyed_mutex )
	{
		const HRESULT acquire_hr = keyed_mutex->AcquireSync( 0, 10 );
		if ( acquire_hr != S_OK )
		{
			char trace_line[ 128 ];
			std::snprintf( trace_line, sizeof( trace_line ), "AcquireSync failed hr=0x%08x", static_cast< unsigned int >( acquire_hr ) );
			DriverLog( "Flow virtual display D3D: %s", trace_line );
			TraceVirtualDisplayCall( trace_line );
			keyed_mutex->Release();
			return;
		}
	}

	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc( &desc );
	if ( !logged_texture_desc_ )
	{
		char trace_line[ 256 ];
		std::snprintf( trace_line, sizeof( trace_line ),
		               "Texture opened handle=0x%llx %ux%u mip=%u array=%u format=%u bind=0x%x misc=0x%x",
		               static_cast< unsigned long long >( present_info.backbufferTextureHandle ),
		               desc.Width, desc.Height, desc.MipLevels, desc.ArraySize,
		               static_cast< unsigned int >( desc.Format ), desc.BindFlags, desc.MiscFlags );
		DriverLog( "Flow virtual display D3D: %s", trace_line );
		TraceVirtualDisplayCall( trace_line );
		logged_texture_desc_ = true;
	}

	if ( flush_texture_ == nullptr )
	{
		D3D11_TEXTURE2D_DESC flush_desc{};
		flush_desc.Width = 1;
		flush_desc.Height = 1;
		flush_desc.MipLevels = 1;
		flush_desc.ArraySize = 1;
		flush_desc.Format = desc.Format;
		flush_desc.SampleDesc.Count = 1;
		flush_desc.Usage = D3D11_USAGE_STAGING;
		flush_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		const HRESULT create_hr = d3d_device_->CreateTexture2D( &flush_desc, nullptr, &flush_texture_ );
		if ( FAILED( create_hr ) )
		{
			char trace_line[ 128 ];
			std::snprintf( trace_line, sizeof( trace_line ), "Create flush texture failed hr=0x%08x",
			               static_cast< unsigned int >( create_hr ) );
			DriverLog( "Flow virtual display D3D: %s", trace_line );
			TraceVirtualDisplayCall( trace_line );
			if ( keyed_mutex != nullptr )
			{
				keyed_mutex->ReleaseSync( 0 );
				keyed_mutex->Release();
			}
			return;
		}
	}

	D3D11_BOX source_box{ 0, 0, 0, 1, 1, 1 };
	d3d_context_->CopySubresourceRegion( flush_texture_, 0, 0, 0, 0, texture, 0, &source_box );
	d3d_context_->Flush();

	if ( present_count == 1 || ( present_count % 300 ) == 0 )
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		uint32_t pixel = 0;
		// DO_NOT_WAIT: Present must never block on the GPU while holding the keyed mutex.
		const HRESULT map_hr = d3d_context_->Map( flush_texture_, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped );
		if ( SUCCEEDED( map_hr ) )
		{
			std::memcpy( &pixel, mapped.pData, std::min< size_t >( sizeof( pixel ), mapped.RowPitch ) );
			d3d_context_->Unmap( flush_texture_, 0 );
		}

		char trace_line[ 160 ];
		std::snprintf( trace_line, sizeof( trace_line ), "Texture probe copied 1x1 present=%llu frameId=%llu pixel=0x%08x mapHr=0x%08x",
		               static_cast< unsigned long long >( present_count ),
		               static_cast< unsigned long long >( present_info.nFrameId ),
		               static_cast< unsigned int >( pixel ),
		               static_cast< unsigned int >( map_hr ) );
		TraceVirtualDisplayCall( trace_line );
	}

	// Creating logs\dump_preview.request asks for a fresh preview of what the HMD shows
	// (checked once a second), so overlay/dashboard state can be inspected without the headset.
	if ( present_count % 75 == 0 )
	{
		static const std::string request = DriverLogPath( "dump_preview.request" );
		if ( GetFileAttributesA( request.c_str() ) != INVALID_FILE_ATTRIBUTES && DeleteFileA( request.c_str() ) )
		{
			dumped_texture_preview_ = false;
		}
	}
	if ( dump_readback_pending_ )
	{
		FinishTexturePreviewDump();
	}
	else if ( present_count >= 300 && !dumped_texture_preview_ )
	{
		DumpTexturePreview( texture, present_info );
	}
	StreamTexturePreview( texture, present_info, present_count );

	if ( keyed_mutex != nullptr )
	{
		keyed_mutex->ReleaseSync( 0 );
		keyed_mutex->Release();
	}
#endif
}

bool FlowVirtualDisplayDevice::EnsureStreamSocketConnected()
{
#ifdef _WIN32
	if ( stream_socket_ != kInvalidFlowSocket )
	{
		return true;
	}

	if ( !stream_wsa_started_ )
	{
		WSADATA wsa_data{};
		if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) != 0 )
		{
			TraceVirtualDisplayCall( "H264 stream WSAStartup failed" );
			return false;
		}
		stream_wsa_started_ = true;
	}

	if ( stream_listen_socket_ == kInvalidFlowSocket )
	{
		SOCKET listen_sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
		if ( listen_sock == INVALID_SOCKET )
		{
			TraceVirtualDisplayCall( "H264 stream listen socket creation failed" );
			return false;
		}

		BOOL reuse = TRUE;
		setsockopt( listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast< const char * >( &reuse ), sizeof( reuse ) );

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons( kStreamPreviewPort );
		address.sin_addr.s_addr = htonl( INADDR_ANY );

		if ( bind( listen_sock, reinterpret_cast< sockaddr * >( &address ), sizeof( address ) ) == SOCKET_ERROR )
		{
			TraceVirtualDisplayCall( "H264 stream bind 0.0.0.0:8001 failed" );
			closesocket( listen_sock );
			return false;
		}
		if ( listen( listen_sock, 1 ) == SOCKET_ERROR )
		{
			TraceVirtualDisplayCall( "H264 stream listen failed" );
			closesocket( listen_sock );
			return false;
		}

		u_long non_blocking = 1;
		ioctlsocket( listen_sock, FIONBIO, &non_blocking );
		stream_listen_socket_ = static_cast< FlowSocketHandle >( listen_sock );
		TraceVirtualDisplayCall( "H264 stream listening on 0.0.0.0:8001" );
	}

	SOCKET sock = accept( static_cast< SOCKET >( stream_listen_socket_ ), nullptr, nullptr );
	if ( sock == INVALID_SOCKET )
	{
		BroadcastDiscoveryIfDue();
		return false;
	}

	u_long blocking = 0;
	ioctlsocket( sock, FIONBIO, &blocking );
	DWORD timeout_ms = 3000;
	setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast< const char * >( &timeout_ms ), sizeof( timeout_ms ) );
	BOOL no_delay = TRUE;
	setsockopt( sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast< const char * >( &no_delay ), sizeof( no_delay ) );

	stream_socket_ = static_cast< FlowSocketHandle >( sock );
	stream_header_sent_ = false;
	TraceVirtualDisplayCall( "H264 stream accepted Flow client on 0.0.0.0:8001" );
	return true;
#else
	return false;
#endif
}

bool FlowVirtualDisplayDevice::SendStreamBytes( const void *data, size_t size )
{
#ifdef _WIN32
	if ( stream_socket_ == kInvalidFlowSocket )
	{
		return false;
	}

	const auto *cursor = static_cast< const char * >( data );
	size_t remaining = size;
	while ( remaining > 0 )
	{
		const int chunk = static_cast< int >( std::min< size_t >( remaining, 64 * 1024 ) );
		const int sent = send( static_cast< SOCKET >( stream_socket_ ), cursor, chunk, 0 );
		if ( sent <= 0 )
		{
			char trace_line[ 160 ];
			std::snprintf( trace_line, sizeof( trace_line ), "H264 stream send failed sent=%d wsa=%d remaining=%llu",
			               sent,
			               WSAGetLastError(),
			               static_cast< unsigned long long >( remaining ) );
			TraceVirtualDisplayCall( trace_line );
			CloseStreamSocket();
			return false;
		}
		cursor += sent;
		remaining -= static_cast< size_t >( sent );
	}
	return true;
#else
	return false;
#endif
}

// Announces "FLOWH264_PC <port>" on UDP 8002 (the same message the Python sender uses)
// so the Flow can find this PC without a hard-coded IP. Runs only while no client is connected.
void FlowVirtualDisplayDevice::BroadcastDiscoveryIfDue()
{
#ifdef _WIN32
	const auto now = std::chrono::steady_clock::now();
	if ( now - last_discovery_broadcast_ < kStreamDiscoveryInterval )
	{
		return;
	}
	last_discovery_broadcast_ = now;

	if ( stream_discovery_socket_ == kInvalidFlowSocket )
	{
		SOCKET udp = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		if ( udp == INVALID_SOCKET )
		{
			return;
		}
		BOOL broadcast = TRUE;
		setsockopt( udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast< const char * >( &broadcast ), sizeof( broadcast ) );
		stream_discovery_socket_ = static_cast< FlowSocketHandle >( udp );
	}

	// Directed broadcast for every IPv4 adapter that is up, plus the limited broadcast.
	std::vector< uint32_t > targets{ INADDR_BROADCAST };
	ULONG buffer_size = 16 * 1024;
	std::vector< uint8_t > buffer( buffer_size );
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG result = GetAdaptersAddresses( AF_INET, flags, nullptr,
	                                     reinterpret_cast< IP_ADAPTER_ADDRESSES * >( buffer.data() ), &buffer_size );
	if ( result == ERROR_BUFFER_OVERFLOW )
	{
		buffer.resize( buffer_size );
		result = GetAdaptersAddresses( AF_INET, flags, nullptr,
		                               reinterpret_cast< IP_ADAPTER_ADDRESSES * >( buffer.data() ), &buffer_size );
	}
	if ( result == NO_ERROR )
	{
		for ( auto *adapter = reinterpret_cast< IP_ADAPTER_ADDRESSES * >( buffer.data() ); adapter != nullptr; adapter = adapter->Next )
		{
			if ( adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK )
			{
				continue;
			}
			for ( auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next )
			{
				const auto *address = reinterpret_cast< const sockaddr_in * >( unicast->Address.lpSockaddr );
				const ULONG prefix = unicast->OnLinkPrefixLength;
				if ( address->sin_family != AF_INET || prefix == 0 || prefix >= 32 )
				{
					continue;
				}
				const uint32_t host = ntohl( address->sin_addr.s_addr );
				const uint32_t mask = 0xffffffffu << ( 32 - prefix );
				const uint32_t directed = host | ~mask;
				if ( std::find( targets.begin(), targets.end(), directed ) == targets.end() )
				{
					targets.push_back( directed );
				}
			}
		}
	}

	char message[ 32 ];
	const int length = std::snprintf( message, sizeof( message ), "FLOWH264_PC %u", static_cast< unsigned int >( kStreamPreviewPort ) );
	for ( const uint32_t target : targets )
	{
		sockaddr_in destination{};
		destination.sin_family = AF_INET;
		destination.sin_port = htons( kStreamDiscoveryPort );
		destination.sin_addr.s_addr = htonl( target );
		sendto( static_cast< SOCKET >( stream_discovery_socket_ ), message, length, 0,
		        reinterpret_cast< const sockaddr * >( &destination ), sizeof( destination ) );
	}

	if ( !logged_discovery_targets_ )
	{
		logged_discovery_targets_ = true;
		std::string line = "FLOWH264 discovery broadcasting on UDP 8002 to";
		for ( const uint32_t target : targets )
		{
			char text[ 24 ];
			std::snprintf( text, sizeof( text ), " %u.%u.%u.%u", target >> 24, ( target >> 16 ) & 0xff, ( target >> 8 ) & 0xff, target & 0xff );
			line += text;
		}
		DriverLog( "%s", line.c_str() );
		TraceVirtualDisplayCall( line.c_str() );
	}
#endif
}

void FlowVirtualDisplayDevice::CloseStreamSocket()
{
#ifdef _WIN32
	if ( stream_socket_ != kInvalidFlowSocket )
	{
		closesocket( static_cast< SOCKET >( stream_socket_ ) );
		stream_socket_ = kInvalidFlowSocket;
	}
	stream_header_sent_ = false;
	SetStreamClientConnected( false );
#endif
}

bool FlowVirtualDisplayDevice::EnsureH264StreamHeader( const std::vector< uint8_t > &packet )
{
#ifdef _WIN32
	if ( stream_header_sent_ )
	{
		return true;
	}

	std::vector< uint8_t > sps;
	std::vector< uint8_t > pps;
	for ( const auto &range : AnnexBNals( packet ) )
	{
		const int type = NalType( packet, range.first );
		if ( type == 7 )
		{
			sps.assign( packet.begin() + range.first, packet.begin() + range.second );
		}
		else if ( type == 8 )
		{
			pps.assign( packet.begin() + range.first, packet.begin() + range.second );
		}
	}

	if ( sps.empty() || pps.empty() )
	{
		return false;
	}

	const char magic[ 8 ] = { 'F', 'L', 'O', 'W', 'H', '2', '6', '4' };
	// Version 4 appends a layout field: 0 = mono screen, 1 = side-by-side stereo eyes.
	// Version 5 adds a u32 Flow pose sequence to every frame header (see SendH264Packet).
	const uint32_t version = HostToBigEndian32( 5 );
	const uint32_t width = HostToBigEndian32( stream_width_ );
	const uint32_t height = HostToBigEndian32( stream_height_ );
	const uint32_t fps = HostToBigEndian32( kStreamPreviewFps );
	const uint32_t layout = HostToBigEndian32( 1 );
	const uint32_t sps_size = HostToBigEndian32( static_cast< uint32_t >( sps.size() ) );
	const uint32_t pps_size = HostToBigEndian32( static_cast< uint32_t >( pps.size() ) );

	if ( !SendStreamBytes( magic, sizeof( magic ) ) ||
	     !SendStreamBytes( &version, sizeof( version ) ) ||
	     !SendStreamBytes( &width, sizeof( width ) ) ||
	     !SendStreamBytes( &height, sizeof( height ) ) ||
	     !SendStreamBytes( &fps, sizeof( fps ) ) ||
	     !SendStreamBytes( &layout, sizeof( layout ) ) ||
	     !SendStreamBytes( &sps_size, sizeof( sps_size ) ) ||
	     !SendStreamBytes( sps.data(), sps.size() ) ||
	     !SendStreamBytes( &pps_size, sizeof( pps_size ) ) ||
	     !SendStreamBytes( pps.data(), pps.size() ) )
	{
		return false;
	}

	stream_header_sent_ = true;
	char trace_line[ 180 ];
	std::snprintf( trace_line, sizeof( trace_line ), "FLOWH264 direct header sent %ux%u@%u sps=%llu pps=%llu",
	               stream_width_,
	               stream_height_,
	               kStreamPreviewFps,
	               static_cast< unsigned long long >( sps.size() ),
	               static_cast< unsigned long long >( pps.size() ) );
	TraceVirtualDisplayCall( trace_line );
	return true;
#else
	return false;
#endif
}

bool FlowVirtualDisplayDevice::SendH264Packet( const std::vector< uint8_t > &packet, uint64_t pts_us, uint64_t encoded_ready_ms,
                                               uint32_t pose_sequence )
{
#ifdef _WIN32
	int vcl_count = 0;
	for ( const auto &range : AnnexBNals( packet ) )
	{
		const int type = NalType( packet, range.first );
		if ( type >= 1 && type <= 5 )
		{
			++vcl_count;
		}
	}
	if ( vcl_count == 0 )
	{
		TraceVirtualDisplayCall( "FLOWH264 direct packet contained no VCL NAL; waiting for encoded frame" );
		return true;
	}

	if ( !EnsureH264StreamHeader( packet ) )
	{
		return true;
	}

	int sent_vcl_count = 0;
	for ( const auto &range : AnnexBNals( packet ) )
	{
		const int type = NalType( packet, range.first );
		if ( type < 1 || type > 5 )
		{
			continue;
		}

		const uint32_t size = static_cast< uint32_t >( range.second - range.first );
		const uint32_t be_size = HostToBigEndian32( size );
		const int64_t be_pts = HostToBigEndianSigned64( static_cast< int64_t >( pts_us ) );
		const int64_t be_encoded_ready = HostToBigEndianSigned64( static_cast< int64_t >( encoded_ready_ms ) );
		const uint64_t send_start_ms = EpochMilliseconds();
		const int64_t be_send_start = HostToBigEndianSigned64( static_cast< int64_t >( send_start_ms ) );
		const uint32_t be_pose_sequence = HostToBigEndian32( pose_sequence );

		if ( !SendStreamBytes( &be_size, sizeof( be_size ) ) ||
		     !SendStreamBytes( &be_pts, sizeof( be_pts ) ) ||
		     !SendStreamBytes( &be_encoded_ready, sizeof( be_encoded_ready ) ) ||
		     !SendStreamBytes( &be_send_start, sizeof( be_send_start ) ) ||
		     !SendStreamBytes( &be_pose_sequence, sizeof( be_pose_sequence ) ) ||
		     !SendStreamBytes( packet.data() + range.first, size ) )
		{
			return false;
		}

		const uint64_t send_end_ms = EpochMilliseconds();
		const int64_t be_send_end = HostToBigEndianSigned64( static_cast< int64_t >( send_end_ms ) );
		if ( !SendStreamBytes( &be_send_end, sizeof( be_send_end ) ) )
		{
			return false;
		}
		++sent_vcl_count;
	}

	if ( sent_vcl_count > 0 && ( present_count_.load() % 300 ) == 0 )
	{
		char trace_line[ 160 ];
		std::snprintf( trace_line, sizeof( trace_line ), "FLOWH264 direct sent %d VCL NAL(s), packet=%llu bytes",
		               sent_vcl_count,
		               static_cast< unsigned long long >( packet.size() ) );
		TraceVirtualDisplayCall( trace_line );
	}
	return true;
#else
	return false;
#endif
}

bool FlowVirtualDisplayDevice::EnsureStreamDownsampleResources( uint32_t source_format )
{
#ifdef _WIN32
	if ( stream_slot_textures_[ 0 ] != nullptr )
	{
		if ( stream_texture_format_ == source_format )
		{
			return true;
		}
		// The encode thread holds NVENC registrations for these textures; recreating them
		// under it is not supported. The compositor backbuffer format does not change in practice.
		TraceVirtualDisplayCall( "Compositor backbuffer format changed; streaming stopped" );
		return false;
	}
	stream_texture_format_ = source_format;

	D3D11_TEXTURE2D_DESC gpu_desc{};
	gpu_desc.Width = stream_width_;
	gpu_desc.Height = stream_height_;
	gpu_desc.MipLevels = 1;
	gpu_desc.ArraySize = 1;
	gpu_desc.Format = static_cast< DXGI_FORMAT >( source_format );
	gpu_desc.SampleDesc.Count = 1;
	gpu_desc.Usage = D3D11_USAGE_DEFAULT;
	gpu_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = S_OK;
	for ( int slot = 0; slot < kStreamSlotCount; ++slot )
	{
		hr = d3d_device_->CreateTexture2D( &gpu_desc, nullptr, &stream_slot_textures_[ slot ] );
		if ( SUCCEEDED( hr ) )
		{
			hr = d3d_device_->CreateRenderTargetView( stream_slot_textures_[ slot ], nullptr, &stream_slot_rtvs_[ slot ] );
		}
		if ( FAILED( hr ) )
		{
			char trace_line[ 128 ];
			std::snprintf( trace_line, sizeof( trace_line ), "Create stream slot %d failed hr=0x%08x", slot, static_cast< unsigned int >( hr ) );
			TraceVirtualDisplayCall( trace_line );
			return false;
		}
	}

	if ( stream_vertex_shader_ == nullptr || stream_pixel_shader_ == nullptr )
	{
		const char *shader_source =
			"Texture2D sourceTex : register(t0);\n"
			"SamplerState sourceSampler : register(s0);\n"
			"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
			"VSOut vs_main(uint id : SV_VertexID) {\n"
			"  float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };\n"
			// D3D clip space has +Y up but texture V runs top-down, so uv.y = 0.5 - pos.y * 0.5.
			"  float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };\n"
			"  VSOut o; o.pos = float4(pos[id], 0.0, 1.0); o.uv = uv[id]; return o;\n"
			"}\n"
			"float4 ps_main(VSOut input) : SV_Target { return sourceTex.Sample(sourceSampler, input.uv); }\n";

		ID3DBlob *vs_blob = nullptr;
		ID3DBlob *ps_blob = nullptr;
		ID3DBlob *error_blob = nullptr;
		hr = D3DCompile( shader_source, std::strlen( shader_source ), nullptr, nullptr, nullptr,
		                 "vs_main", "vs_4_0", 0, 0, &vs_blob, &error_blob );
		SafeReleaseD3DBlob( error_blob );
		if ( FAILED( hr ) )
		{
			TraceVirtualDisplayCall( "Compile stream vertex shader failed" );
			SafeReleaseD3DBlob( vs_blob );
			return false;
		}

		hr = D3DCompile( shader_source, std::strlen( shader_source ), nullptr, nullptr, nullptr,
		                 "ps_main", "ps_4_0", 0, 0, &ps_blob, &error_blob );
		SafeReleaseD3DBlob( error_blob );
		if ( FAILED( hr ) )
		{
			TraceVirtualDisplayCall( "Compile stream pixel shader failed" );
			SafeReleaseD3DBlob( vs_blob );
			SafeReleaseD3DBlob( ps_blob );
			return false;
		}

		hr = d3d_device_->CreateVertexShader( vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &stream_vertex_shader_ );
		if ( SUCCEEDED( hr ) )
		{
			hr = d3d_device_->CreatePixelShader( ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &stream_pixel_shader_ );
		}
		SafeReleaseD3DBlob( vs_blob );
		SafeReleaseD3DBlob( ps_blob );
		if ( FAILED( hr ) )
		{
			TraceVirtualDisplayCall( "Create stream shader failed" );
			return false;
		}
	}

	if ( stream_sampler_ == nullptr )
	{
		D3D11_SAMPLER_DESC sampler_desc{};
		sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
		hr = d3d_device_->CreateSamplerState( &sampler_desc, &stream_sampler_ );
		if ( FAILED( hr ) )
		{
			TraceVirtualDisplayCall( "Create stream sampler failed" );
			return false;
		}
	}

	TraceVirtualDisplayCall( "Created GPU downsample resources for BGRA stream" );
	return true;
#else
	return false;
#endif
}

bool FlowVirtualDisplayDevice::RenderStreamDownsample( ID3D11Texture2D *texture, int slot )
{
#ifdef _WIN32
	if ( texture == nullptr )
	{
		return false;
	}

	ID3D11ShaderResourceView *source_view = nullptr;
	HRESULT hr = d3d_device_->CreateShaderResourceView( texture, nullptr, &source_view );
	if ( FAILED( hr ) )
	{
		char trace_line[ 128 ];
		std::snprintf( trace_line, sizeof( trace_line ), "Create source SRV failed hr=0x%08x", static_cast< unsigned int >( hr ) );
		TraceVirtualDisplayCall( trace_line );
		return false;
	}

	D3D11_VIEWPORT viewport{};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast< float >( stream_width_ );
	viewport.Height = static_cast< float >( stream_height_ );
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	d3d_context_->IASetInputLayout( nullptr );
	d3d_context_->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	d3d_context_->RSSetViewports( 1, &viewport );
	d3d_context_->OMSetRenderTargets( 1, &stream_slot_rtvs_[ slot ], nullptr );
	d3d_context_->VSSetShader( stream_vertex_shader_, nullptr, 0 );
	d3d_context_->PSSetShader( stream_pixel_shader_, nullptr, 0 );
	d3d_context_->PSSetShaderResources( 0, 1, &source_view );
	d3d_context_->PSSetSamplers( 0, 1, &stream_sampler_ );
	d3d_context_->Draw( 3, 0 );

	ID3D11ShaderResourceView *null_srv = nullptr;
	d3d_context_->PSSetShaderResources( 0, 1, &null_srv );
	source_view->Release();
	return true;
#else
	return false;
#endif
}

void FlowVirtualDisplayDevice::StreamTexturePreview( ID3D11Texture2D *texture, const vr::PresentInfo_t &present_info, uint64_t present_count )
{
#ifdef _WIN32
	// Runs on the compositor's Present thread with the backbuffer's keyed mutex held, so it
	// only downsamples into a free slot and hands it off. NVENC and the socket live on the
	// encode thread; doing them here (~12-15 ms) made SteamVR fall from 75 Hz to ~37 Hz.
	if ( texture == nullptr )
	{
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	++stats_calls_;
	if ( !stream_client_connected_.load() )
	{
		return;
	}

	// Present arrives every ~13.3 ms with jitter; accept frames up to half an interval early
	// so a slightly early Present is not dropped.
	const auto frame_interval = std::chrono::duration_cast< std::chrono::steady_clock::duration >( kStreamFrameInterval );
	if ( now + frame_interval / 2 < next_stream_frame_ )
	{
		++stats_throttled_;
		return;
	}

	// The compositor reprojects to the newest pose it has right before Present, so the pose
	// handed to SteamVR now is the best estimate of what this frame was rendered with.
	const uint32_t pose_sequence = g_flow_pose_sequence_in_use.load();

	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc( &desc );
	if ( desc.Width == 0 || desc.Height == 0 || !EnsureStreamDownsampleResources( desc.Format ) )
	{
		return;
	}

	int slot = -1;
	{
		std::lock_guard< std::mutex > lock( encode_mutex_ );
		for ( int candidate = 0; candidate < kStreamSlotCount; ++candidate )
		{
			if ( candidate != encoding_slot_ && candidate != pending_slot_ )
			{
				slot = candidate;
				break;
			}
		}
	}
	if ( slot < 0 || !RenderStreamDownsample( texture, slot ) )
	{
		return;
	}
	d3d_context_->Flush();

	{
		std::lock_guard< std::mutex > lock( encode_mutex_ );
		if ( pending_slot_ >= 0 )
		{
			++stats_replaced_; // encoder still busy; drop the older frame, keep latency low
		}
		pending_slot_ = slot;
		pending_pts_us_ = ( present_count * 1000000ull ) / kStreamPreviewFps;
		pending_pose_sequence_ = pose_sequence;
	}
	encode_cv_.notify_one();

	++stats_captured_;
	stats_capture_us_ += static_cast< uint64_t >(
		std::chrono::duration_cast< std::chrono::microseconds >( std::chrono::steady_clock::now() - now ).count() );
	// Advance on the fixed cadence; after a stall, restart the schedule from now.
	next_stream_frame_ = ( std::max )( next_stream_frame_ + frame_interval, now ); // parens dodge the windows.h max macro
#endif
}

// Tracks whether the Flow is receiving the stream and publishes it on the HMD device
// (kProp_FlowStreamConnected_Bool) for the dashboard helper.
void FlowVirtualDisplayDevice::SetStreamClientConnected( bool connected )
{
	// Reached at the latest from Deactivate/Cleanup, while the driver context is still valid.
	if ( stream_client_connected_.exchange( connected ) == connected )
	{
		return;
	}
	const vr::PropertyContainerHandle_t hmd = vr::VRProperties()->TrackedDeviceToPropertyContainer( vr::k_unTrackedDeviceIndex_Hmd );
	vr::VRProperties()->SetBoolProperty( hmd, static_cast< vr::ETrackedDeviceProperty >( kProp_FlowStreamConnected_Bool ), connected );
	TraceVirtualDisplayCall( connected ? "Flow stream client connected" : "Flow stream client disconnected" );
}

void FlowVirtualDisplayDevice::StartEncodeThread()
{
#ifdef _WIN32
	if ( encode_thread_.joinable() )
	{
		return;
	}
	{
		std::lock_guard< std::mutex > lock( encode_mutex_ );
		encode_stop_ = false;
		pending_slot_ = -1;
		encoding_slot_ = -1;
	}
	encode_thread_ = std::thread( &FlowVirtualDisplayDevice::EncodeThreadMain, this );
#endif
}

void FlowVirtualDisplayDevice::StopEncodeThread()
{
#ifdef _WIN32
	{
		std::lock_guard< std::mutex > lock( encode_mutex_ );
		encode_stop_ = true;
	}
	encode_cv_.notify_all();
	if ( encode_thread_.joinable() )
	{
		encode_thread_.join();
	}
	SetStreamClientConnected( false );
#endif
}

// Owns the client socket and NVENC: accepts the Flow (broadcasting discovery meanwhile),
// then encodes and sends whichever slot Present handed over most recently.
void FlowVirtualDisplayDevice::EncodeThreadMain()
{
#ifdef _WIN32
	TraceVirtualDisplayCall( "FLOWH264 encode thread started" );
	while ( true )
	{
		if ( stream_socket_ == kInvalidFlowSocket )
		{
			if ( EnsureStreamSocketConnected() )
			{
				if ( nvenc_encoder_ != nullptr )
				{
					nvenc_encoder_->RequestKeyframe(); // new client can start decoding at once
				}
				SetStreamClientConnected( true );
			}
		}

		int slot = -1;
		uint64_t pts_us = 0;
		uint32_t pose_sequence = 0;
		{
			std::unique_lock< std::mutex > lock( encode_mutex_ );
			const auto wait = stream_client_connected_.load() ? std::chrono::milliseconds( 100 ) : std::chrono::milliseconds( 20 );
			encode_cv_.wait_for( lock, wait, [ this ] { return encode_stop_ || pending_slot_ >= 0; } );
			if ( encode_stop_ )
			{
				break;
			}
			if ( pending_slot_ >= 0 )
			{
				slot = pending_slot_;
				pts_us = pending_pts_us_;
				pose_sequence = pending_pose_sequence_;
				pending_slot_ = -1;
				encoding_slot_ = slot;
			}
		}

		if ( slot >= 0 )
		{
			EncodeAndSendSlot( slot, pts_us, pose_sequence );
			std::lock_guard< std::mutex > lock( encode_mutex_ );
			encoding_slot_ = -1;
		}
		LogStreamStatsIfDue( std::chrono::steady_clock::now() );
	}
	TraceVirtualDisplayCall( "FLOWH264 encode thread stopped" );
#endif
}

void FlowVirtualDisplayDevice::EncodeAndSendSlot( int slot, uint64_t pts_us, uint32_t pose_sequence )
{
#ifdef _WIN32
	if ( nvenc_failed_ || stream_socket_ == kInvalidFlowSocket )
	{
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	if ( nvenc_encoder_ == nullptr )
	{
		std::lock_guard< std::mutex > lock( d3d_mutex_ );
		nvenc_encoder_ = std::make_unique< FlowNvencEncoder >();
		if ( !nvenc_encoder_->Initialize( d3d_device_, stream_width_, stream_height_, kStreamPreviewFps, stream_bitrate_, nvenc_preset_ ) )
		{
			nvenc_failed_ = true;
			TraceVirtualDisplayCall( "FLOWH264 direct NVENC initialize failed" );
			CloseStreamSocket();
			return;
		}
	}

	std::vector< uint8_t > packet;
	if ( !nvenc_encoder_->EncodeTexture( stream_slot_textures_[ slot ], pts_us, packet, &d3d_mutex_ ) )
	{
		TraceVirtualDisplayCall( "FLOWH264 direct NVENC encode failed" );
		nvenc_failed_ = true;
		CloseStreamSocket();
		return;
	}
	if ( packet.empty() )
	{
		return;
	}
	const auto encode_done = std::chrono::steady_clock::now();

	if ( !SendH264Packet( packet, pts_us, EpochMilliseconds(), pose_sequence ) )
	{
		CloseStreamSocket();
		return;
	}
	const auto send_done = std::chrono::steady_clock::now();

	++stats_sent_;
	const double encode_ms = std::chrono::duration< double, std::milli >( encode_done - start ).count();
	stats_encode_ms_ += encode_ms;
	stats_encode_max_ms_ = ( std::max )( stats_encode_max_ms_, encode_ms );
	stats_send_ms_ += std::chrono::duration< double, std::milli >( send_done - encode_done ).count();
#endif
}

// Every 2 s (from the encode thread): SteamVR's present rate, how many frames Present
// handed over, and how long encoding/sending took off the compositor's thread.
void FlowVirtualDisplayDevice::LogStreamStatsIfDue( std::chrono::steady_clock::time_point now )
{
#ifdef _WIN32
	if ( stats_window_start_ == std::chrono::steady_clock::time_point{} )
	{
		stats_window_start_ = now;
		stats_present_base_ = present_count_.load();
		return;
	}
	const double seconds = std::chrono::duration< double >( now - stats_window_start_ ).count();
	if ( seconds < 2.0 )
	{
		return;
	}
	const uint64_t presents = present_count_.load() - stats_present_base_;
	const uint32_t captured = stats_captured_.exchange( 0 );
	const uint64_t capture_us = stats_capture_us_.exchange( 0 );
	char line[ 320 ];
	std::snprintf( line, sizeof( line ),
	               "stream stats %.1fs: presentHz=%.1f calls=%u throttled=%u capturedFps=%.1f captureAvgUs=%.0f replaced=%u "
	               "sentFps=%.1f encodeAvgMs=%.2f encodeMaxMs=%.2f sendAvgMs=%.2f client=%d",
	               seconds, presents / seconds, stats_calls_.exchange( 0 ), stats_throttled_.exchange( 0 ),
	               captured / seconds, captured > 0 ? static_cast< double >( capture_us ) / captured : 0.0,
	               stats_replaced_.exchange( 0 ), stats_sent_ / seconds,
	               stats_sent_ > 0 ? stats_encode_ms_ / stats_sent_ : 0.0, stats_encode_max_ms_,
	               stats_sent_ > 0 ? stats_send_ms_ / stats_sent_ : 0.0, stream_client_connected_.load() ? 1 : 0 );
	TraceVirtualDisplayCall( line );
	stats_window_start_ = now;
	stats_present_base_ = present_count_.load();
	stats_sent_ = 0;
	stats_encode_ms_ = stats_encode_max_ms_ = stats_send_ms_ = 0.0;
#endif
}

void FlowVirtualDisplayDevice::DumpTexturePreview( ID3D11Texture2D *texture, const vr::PresentInfo_t &present_info )
{
#ifdef _WIN32
	if ( texture == nullptr || dumped_texture_preview_ )
	{
		return;
	}

	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc( &desc );
	if ( desc.Width == 0 || desc.Height == 0 )
	{
		return;
	}

	if ( dump_texture_ == nullptr )
	{
		D3D11_TEXTURE2D_DESC dump_desc = desc;
		dump_desc.BindFlags = 0;
		dump_desc.MiscFlags = 0;
		dump_desc.Usage = D3D11_USAGE_STAGING;
		dump_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		const HRESULT create_hr = d3d_device_->CreateTexture2D( &dump_desc, nullptr, &dump_texture_ );
		if ( FAILED( create_hr ) )
		{
			char trace_line[ 128 ];
			std::snprintf( trace_line, sizeof( trace_line ), "Create dump texture failed hr=0x%08x",
			               static_cast< unsigned int >( create_hr ) );
			DriverLog( "Flow virtual display D3D: %s", trace_line );
			TraceVirtualDisplayCall( trace_line );
			return;
		}
	}

	d3d_context_->CopyResource( dump_texture_, texture );
	d3d_context_->Flush();
	dump_readback_pending_ = true;
	dump_frame_id_ = present_info.nFrameId;
#endif
}

// Writes the queued preview copy to flow_compositor_preview.ppm once the GPU has finished it.
void FlowVirtualDisplayDevice::FinishTexturePreviewDump()
{
#ifdef _WIN32
	D3D11_TEXTURE2D_DESC desc{};
	dump_texture_->GetDesc( &desc );

	D3D11_MAPPED_SUBRESOURCE mapped{};
	const HRESULT map_hr = d3d_context_->Map( dump_texture_, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped );
	if ( map_hr == DXGI_ERROR_WAS_STILL_DRAWING )
	{
		return; // try again next Present
	}
	dump_readback_pending_ = false;
	if ( FAILED( map_hr ) )
	{
		char trace_line[ 128 ];
		std::snprintf( trace_line, sizeof( trace_line ), "Map dump texture failed hr=0x%08x",
		               static_cast< unsigned int >( map_hr ) );
		DriverLog( "Flow virtual display D3D: %s", trace_line );
		TraceVirtualDisplayCall( trace_line );
		return;
	}

	const uint32_t preview_width = std::min< uint32_t >( 640, desc.Width );
	const uint32_t preview_height = std::max< uint32_t >( 1, preview_width * desc.Height / desc.Width );
	const uint32_t step_x = std::max< uint32_t >( 1, desc.Width / preview_width );
	const uint32_t step_y = std::max< uint32_t >( 1, desc.Height / preview_height );

	std::ofstream ppm( DriverLogPath( "flow_compositor_preview.ppm" ), std::ios::binary );
	ppm << "P6\n" << preview_width << " " << preview_height << "\n255\n";
	const auto *base = static_cast< const uint8_t * >( mapped.pData );
	for ( uint32_t y = 0; y < preview_height; ++y )
	{
		const uint32_t src_y = std::min< uint32_t >( desc.Height - 1, y * step_y );
		const auto *row = base + mapped.RowPitch * src_y;
		for ( uint32_t x = 0; x < preview_width; ++x )
		{
			const uint32_t src_x = std::min< uint32_t >( desc.Width - 1, x * step_x );
			const auto *pixel = row + src_x * 4;
			const char rgb[ 3 ] = {
				static_cast< char >( pixel[ 0 ] ),
				static_cast< char >( pixel[ 1 ] ),
				static_cast< char >( pixel[ 2 ] ),
			};
			ppm.write( rgb, sizeof( rgb ) );
		}
	}

	d3d_context_->Unmap( dump_texture_, 0 );
	dumped_texture_preview_ = true;

	char trace_line[ 220 ];
	std::snprintf( trace_line, sizeof( trace_line ),
	               "Dumped compositor preview %ux%u from %ux%u frameId=%llu",
	               preview_width, preview_height, desc.Width, desc.Height,
	               static_cast< unsigned long long >( dump_frame_id_ ) );
	DriverLog( "Flow virtual display D3D: %s", trace_line );
	TraceVirtualDisplayCall( trace_line );
#endif
}

void FlowVirtualDisplayDevice::ReleaseD3DResources()
{
#ifdef _WIN32
	StopEncodeThread(); // before d3d_mutex_: the thread takes it to submit NVENC work
	std::lock_guard< std::mutex > lock( d3d_mutex_ );
	if ( owns_stream_ )
	{
		FlowVirtualDisplayDevice *expected_owner = this;
		g_stream_owner.compare_exchange_strong( expected_owner, nullptr );
		owns_stream_ = false;
	}
	nvenc_encoder_.reset();
	nvenc_failed_ = false;
	if ( flush_texture_ != nullptr )
	{
		flush_texture_->Release();
		flush_texture_ = nullptr;
	}
	if ( dump_texture_ != nullptr )
	{
		dump_texture_->Release();
		dump_texture_ = nullptr;
	}
	for ( int slot = 0; slot < kStreamSlotCount; ++slot )
	{
		if ( stream_slot_rtvs_[ slot ] != nullptr )
		{
			stream_slot_rtvs_[ slot ]->Release();
			stream_slot_rtvs_[ slot ] = nullptr;
		}
		if ( stream_slot_textures_[ slot ] != nullptr )
		{
			stream_slot_textures_[ slot ]->Release();
			stream_slot_textures_[ slot ] = nullptr;
		}
	}
	if ( stream_vertex_shader_ != nullptr )
	{
		stream_vertex_shader_->Release();
		stream_vertex_shader_ = nullptr;
	}
	if ( stream_pixel_shader_ != nullptr )
	{
		stream_pixel_shader_->Release();
		stream_pixel_shader_ = nullptr;
	}
	if ( stream_sampler_ != nullptr )
	{
		stream_sampler_->Release();
		stream_sampler_ = nullptr;
	}
	stream_texture_format_ = 0;
	SafeReleaseTextureMap( shared_textures_ );
	stream_buffer_.clear();
	CloseStreamSocket();
	if ( stream_listen_socket_ != kInvalidFlowSocket )
	{
		closesocket( static_cast< SOCKET >( stream_listen_socket_ ) );
		stream_listen_socket_ = kInvalidFlowSocket;
	}
	if ( stream_discovery_socket_ != kInvalidFlowSocket )
	{
		closesocket( static_cast< SOCKET >( stream_discovery_socket_ ) );
		stream_discovery_socket_ = kInvalidFlowSocket;
	}
	if ( stream_wsa_started_ )
	{
		WSACleanup();
		stream_wsa_started_ = false;
	}
	if ( d3d_context_ != nullptr )
	{
		d3d_context_->Release();
		d3d_context_ = nullptr;
	}
	if ( d3d_device_ != nullptr )
	{
		d3d_device_->Release();
		d3d_device_ = nullptr;
	}
	if ( dxgi_factory_ != nullptr )
	{
		dxgi_factory_->Release();
		dxgi_factory_ = nullptr;
	}
	logged_texture_desc_ = false;
	dumped_texture_preview_ = false;
	dump_readback_pending_ = false;
#endif
}

void FlowVirtualDisplayDevice::WaitForPresent()
{
	static std::atomic< uint64_t > wait_count{ 0 };
	const uint64_t count = ++wait_count;
	if ( count == 1 || ( count % 300 ) == 0 )
	{
		DriverLog( "Flow virtual display WaitForPresent count=%llu presentCount=%llu",
		           static_cast< unsigned long long >( count ),
		           static_cast< unsigned long long >( present_count_.load() ) );
		char trace_line[ 256 ];
		std::snprintf( trace_line, sizeof( trace_line ), "WaitForPresent count=%llu presentCount=%llu",
		               static_cast< unsigned long long >( count ),
		               static_cast< unsigned long long >( present_count_.load() ) );
		TraceVirtualDisplayCall( trace_line );
	}

	const auto now = std::chrono::steady_clock::now();
	const auto elapsed = now - start_time_;
	const uint64_t next_frame = static_cast< uint64_t >(
		std::chrono::duration< double >( elapsed ).count() * kDisplayFrequencyHz ) + 1;
	const auto next_vsync = start_time_ + std::chrono::duration_cast< std::chrono::steady_clock::duration >( kFrameDuration * next_frame );

	if ( next_vsync > now )
	{
		std::this_thread::sleep_until( next_vsync );
	}

	last_vsync_ = std::chrono::steady_clock::now();
	vsync_counter_ = next_frame;
}

bool FlowVirtualDisplayDevice::GetTimeSinceLastVsync( float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter )
{
	const auto now = std::chrono::steady_clock::now();
	const auto seconds_since_vsync = std::chrono::duration< float >( now - last_vsync_ ).count();

	if ( pfSecondsSinceLastVsync != nullptr )
	{
		*pfSecondsSinceLastVsync = seconds_since_vsync < 0.0f ? 0.0f : seconds_since_vsync;
	}
	if ( pulFrameCounter != nullptr )
	{
		*pulFrameCounter = vsync_counter_.load();
	}
	return true;
}

const std::string &FlowVirtualDisplayDevice::MyGetSerialNumber() const
{
	return serial_number_;
}
