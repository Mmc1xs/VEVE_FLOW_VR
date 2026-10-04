#include "desktop_layer_streamer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <d3d11.h>
#include <openvr.h>

#include "driverlog.h"
#include "flow_nvenc_encoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace
{
	constexpr uint16_t kDesktopPort = 8005;
	// The overlay texture is re-fetched this often (Desktop+ recreates it when monitors change).
	constexpr int64_t kSourceRefreshMs = 2000;

	template < typename T >
	void SafeRelease( T *&p )
	{
		if ( p != nullptr )
		{
			p->Release();
			p = nullptr;
		}
	}

	int64_t NowMs()
	{
		return std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now().time_since_epoch() ).count();
	}

	int64_t EpochMs()
	{
		return std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::system_clock::now().time_since_epoch() ).count();
	}

	int64_t SteadyUs()
	{
		return std::chrono::duration_cast< std::chrono::microseconds >( std::chrono::steady_clock::now().time_since_epoch() ).count();
	}

	void PutU32( std::vector< uint8_t > &out, uint32_t value )
	{
		for ( int shift = 24; shift >= 0; shift -= 8 )
		{
			out.push_back( static_cast< uint8_t >( value >> shift ) );
		}
	}

	void PutI64( std::vector< uint8_t > &out, int64_t value )
	{
		for ( int shift = 56; shift >= 0; shift -= 8 )
		{
			out.push_back( static_cast< uint8_t >( static_cast< uint64_t >( value ) >> shift ) );
		}
	}

	bool SendAll( SOCKET sock, const void *data, size_t size )
	{
		const auto *cursor = static_cast< const char * >( data );
		while ( size > 0 )
		{
			const int sent = send( sock, cursor, static_cast< int >( std::min< size_t >( size, 64 * 1024 ) ), 0 );
			if ( sent <= 0 )
			{
				return false;
			}
			cursor += sent;
			size -= static_cast< size_t >( sent );
		}
		return true;
	}

	// The Flow never sends on this socket, so "readable" means it closed.
	bool ClientClosed( SOCKET sock )
	{
		fd_set read_set;
		FD_ZERO( &read_set );
		FD_SET( sock, &read_set );
		timeval zero{};
		if ( select( 0, &read_set, nullptr, nullptr, &zero ) <= 0 )
		{
			return false;
		}
		char byte;
		return recv( sock, &byte, 1, 0 ) <= 0;
	}

	// Annex B NAL units of an encoded packet as [payload start, end) (start code skipped).
	std::vector< std::pair< size_t, size_t > > NalUnits( const std::vector< uint8_t > &data )
	{
		std::vector< size_t > starts;
		for ( size_t i = 0; i + 3 <= data.size(); ++i )
		{
			if ( data[ i ] == 0 && data[ i + 1 ] == 0 && data[ i + 2 ] == 1 )
			{
				starts.push_back( i + 3 );
				i += 2;
			}
		}
		std::vector< std::pair< size_t, size_t > > units;
		for ( size_t n = 0; n < starts.size(); ++n )
		{
			size_t end = n + 1 < starts.size() ? starts[ n + 1 ] - 3 : data.size();
			while ( end > starts[ n ] && data[ end - 1 ] == 0 )
			{
				--end; // zero byte of a 4-byte start code (or trailing zeros)
			}
			units.emplace_back( starts[ n ], end );
		}
		return units;
	}

	// Stream header from the first IDR's SPS/PPS; false if this packet has none.
	bool SendHeader( SOCKET sock, const std::vector< uint8_t > &packet, uint32_t width, uint32_t height, uint32_t fps )
	{
		std::vector< uint8_t > sps, pps;
		for ( const auto &unit : NalUnits( packet ) )
		{
			const int type = packet[ unit.first ] & 0x1F;
			std::vector< uint8_t > nal = { 0, 0, 0, 1 }; // the Flow's MediaCodec takes csd with start codes
			nal.insert( nal.end(), packet.begin() + unit.first, packet.begin() + unit.second );
			if ( type == 7 )
			{
				sps = nal;
			}
			else if ( type == 8 )
			{
				pps = nal;
			}
		}
		if ( sps.empty() || pps.empty() )
		{
			return false;
		}
		std::vector< uint8_t > header( { 'F', 'L', 'O', 'W', 'H', '2', '6', '4' } );
		PutU32( header, 4 );
		PutU32( header, width );
		PutU32( header, height );
		PutU32( header, fps );
		PutU32( header, 0 ); // layout: mono
		PutU32( header, static_cast< uint32_t >( sps.size() ) );
		header.insert( header.end(), sps.begin(), sps.end() );
		PutU32( header, static_cast< uint32_t >( pps.size() ) );
		header.insert( header.end(), pps.begin(), pps.end() );
		return SendAll( sock, header.data(), header.size() );
	}

	bool SendFrame( SOCKET sock, const std::vector< uint8_t > &packet, int64_t pts_us )
	{
		const int64_t encoded_ms = EpochMs();
		for ( const auto &unit : NalUnits( packet ) )
		{
			const int type = packet[ unit.first ] & 0x1F;
			if ( type < 1 || type > 5 )
			{
				continue; // SPS/PPS/SEI: already in the header
			}
			std::vector< uint8_t > out;
			PutU32( out, static_cast< uint32_t >( unit.second - unit.first + 4 ) );
			PutI64( out, pts_us );
			PutI64( out, encoded_ms );
			PutI64( out, EpochMs() );
			out.insert( out.end(), { 0, 0, 0, 1 } );
			out.insert( out.end(), packet.begin() + unit.first, packet.begin() + unit.second );
			PutI64( out, EpochMs() );
			if ( !SendAll( sock, out.data(), out.size() ) )
			{
				return false;
			}
		}
		return true;
	}

	// Desktop+'s overlay texture opened on our device, and the encoder fed from it.
	class PanelSource
	{
	public:
		~PanelSource() { Close(); }

		bool Init()
		{
			if ( FAILED( D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
			                                D3D11_SDK_VERSION, &device_, nullptr, &context_ ) ) )
			{
				DriverLog( "desktop layer: no D3D11 device" );
				return false;
			}
			// Any resource of our device: tells OpenVR which device to open the overlay texture on.
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = 4;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			return SUCCEEDED( device_->CreateTexture2D( &desc, nullptr, &device_ref_ ) );
		}

		void Close()
		{
			ReleaseSource();
			encoder_.reset();
			SafeRelease( frame_ );
			SafeRelease( device_ref_ );
			SafeRelease( context_ );
			SafeRelease( device_ );
		}

		uint32_t Width() const { return width_; }
		uint32_t Height() const { return height_; }
		void RequestKeyframe()
		{
			if ( encoder_ )
			{
				encoder_->RequestKeyframe();
			}
		}

		enum class Result
		{
			None,
			Frame,
			SizeChanged,
		};

		// Copies the part of the Desktop+ overlay's texture the panel shows and encodes it.
		Result Capture( vr::VROverlayHandle_t handle, std::vector< uint8_t > &packet, uint32_t fps, uint32_t bitrate )
		{
			const int64_t now = NowMs();
			if ( source_ == nullptr || handle != source_handle_ || now >= next_refresh_ms_ )
			{
				if ( !AcquireSource( handle ) )
				{
					return Result::None;
				}
				next_refresh_ms_ = now + kSourceRefreshMs;
			}

			// Part shown, widened to whole even pixels (Desktop+ insets its bounds by about a pixel).
			vr::VRTextureBounds_t bounds{ 0.f, 0.f, 1.f, 1.f };
			vr::VROverlay()->GetOverlayTextureBounds( handle, &bounds );
			const float u0 = std::min( bounds.uMin, bounds.uMax ), u1 = std::max( bounds.uMin, bounds.uMax );
			const float v0 = std::min( bounds.vMin, bounds.vMax ), v1 = std::max( bounds.vMin, bounds.vMax );
			UINT x0 = static_cast< UINT >( std::floor( u0 * source_width_ ) ) & ~1u;
			UINT y0 = static_cast< UINT >( std::floor( v0 * source_height_ ) ) & ~1u;
			UINT x1 = std::min( ( static_cast< UINT >( std::ceil( u1 * source_width_ ) ) + 1 ) & ~1u, source_width_ & ~1u );
			UINT y1 = std::min( ( static_cast< UINT >( std::ceil( v1 * source_height_ ) ) + 1 ) & ~1u, source_height_ & ~1u );
			if ( x1 <= x0 + 16 || y1 <= y0 + 16 )
			{
				return Result::None;
			}
			const uint32_t width = x1 - x0;
			const uint32_t height = y1 - y0;
			if ( !encoder_ || width != width_ || height != height_ )
			{
				const bool had_encoder = encoder_ != nullptr;
				if ( !CreateEncoder( width, height, fps, bitrate ) )
				{
					return Result::None;
				}
				if ( had_encoder )
				{
					return Result::SizeChanged;
				}
			}

			D3D11_BOX box{ x0, y0, 0, x1, y1, 1 };
			context_->CopySubresourceRegion( frame_, 0, 0, 0, 0, source_, 0, &box );
			packet.clear();
			if ( !encoder_->EncodeTexture( frame_, static_cast< uint64_t >( SteadyUs() ), packet ) )
			{
				DriverLog( "desktop layer: encode failed: %s", encoder_->LastError().c_str() );
				encoder_.reset();
				return Result::None;
			}
			return packet.empty() ? Result::None : Result::Frame;
		}

	private:
		bool AcquireSource( vr::VROverlayHandle_t handle )
		{
			ReleaseSource();
			void *native = nullptr;
			uint32_t width = 0, height = 0, format = 0;
			vr::ETextureType type = vr::TextureType_Invalid;
			vr::EColorSpace space = vr::ColorSpace_Auto;
			vr::VRTextureBounds_t bounds{};
			const vr::EVROverlayError error =
				vr::VROverlay()->GetOverlayTexture( handle, &native, device_ref_, &width, &height, &format, &type, &space, &bounds );
			if ( error != vr::VROverlayError_None || native == nullptr )
			{
				return false;
			}
			ID3D11Resource *resource = nullptr;
			static_cast< ID3D11ShaderResourceView * >( native )->GetResource( &resource );
			if ( resource == nullptr ||
			     FAILED( resource->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &source_ ) ) ) )
			{
				SafeRelease( resource );
				vr::VROverlay()->ReleaseNativeOverlayHandle( handle, native );
				return false;
			}
			resource->Release();
			D3D11_TEXTURE2D_DESC desc{};
			source_->GetDesc( &desc );
			source_width_ = desc.Width;
			source_height_ = desc.Height;
			source_native_ = native;
			source_handle_ = handle;
			return true;
		}

		void ReleaseSource()
		{
			SafeRelease( source_ );
			if ( source_native_ != nullptr )
			{
				vr::VROverlay()->ReleaseNativeOverlayHandle( source_handle_, source_native_ );
				source_native_ = nullptr;
			}
			source_handle_ = vr::k_ulOverlayHandleInvalid;
		}

		bool CreateEncoder( uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate )
		{
			encoder_.reset();
			SafeRelease( frame_ );
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // same family as Desktop+'s sRGB texture: a plain byte copy
			desc.SampleDesc.Count = 1;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if ( FAILED( device_->CreateTexture2D( &desc, nullptr, &frame_ ) ) )
			{
				return false;
			}
			encoder_ = std::make_unique< FlowNvencEncoder >();
			if ( !encoder_->Initialize( device_, width, height, fps, bitrate, 4 ) )
			{
				DriverLog( "desktop layer: NVENC init failed: %s", encoder_->LastError().c_str() );
				encoder_.reset();
				return false;
			}
			width_ = width;
			height_ = height;
			DriverLog( "desktop layer: encoding Desktop+ panel picture %ux%u", width, height );
			return true;
		}

		ID3D11Device *device_ = nullptr;
		ID3D11DeviceContext *context_ = nullptr;
		ID3D11Texture2D *device_ref_ = nullptr;
		vr::VROverlayHandle_t source_handle_ = vr::k_ulOverlayHandleInvalid;
		void *source_native_ = nullptr;
		ID3D11Texture2D *source_ = nullptr;
		UINT source_width_ = 0;
		UINT source_height_ = 0;
		int64_t next_refresh_ms_ = 0;
		ID3D11Texture2D *frame_ = nullptr;
		std::unique_ptr< FlowNvencEncoder > encoder_;
		uint32_t width_ = 0;
		uint32_t height_ = 0;
	};
} // namespace

DesktopLayerStreamer::DesktopLayerStreamer( uint32_t bitrate, uint32_t fps ) : bitrate_( bitrate ), fps_( fps )
{
	thread_ = std::thread( &DesktopLayerStreamer::Run, this );
}

DesktopLayerStreamer::~DesktopLayerStreamer()
{
	stop_ = true;
	if ( thread_.joinable() )
	{
		thread_.join();
	}
}

void DesktopLayerStreamer::Run()
{
	PanelSource source;
	if ( !source.Init() )
	{
		return;
	}
	SOCKET listen_sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	BOOL reuse = TRUE;
	setsockopt( listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast< const char * >( &reuse ), sizeof( reuse ) );
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons( kDesktopPort );
	address.sin_addr.s_addr = htonl( INADDR_ANY );
	if ( listen_sock == INVALID_SOCKET || bind( listen_sock, reinterpret_cast< sockaddr * >( &address ), sizeof( address ) ) == SOCKET_ERROR ||
	     listen( listen_sock, 1 ) == SOCKET_ERROR )
	{
		DriverLog( "desktop layer: listen on 0.0.0.0:%u failed (wsa=%d)", kDesktopPort, WSAGetLastError() );
		if ( listen_sock != INVALID_SOCKET )
		{
			closesocket( listen_sock );
		}
		return;
	}
	u_long non_blocking = 1;
	ioctlsocket( listen_sock, FIONBIO, &non_blocking );
	DriverLog( "desktop layer: listening on 0.0.0.0:%u (%u Mbps, %u fps)", kDesktopPort, bitrate_ / 1000000, fps_ );

	SOCKET client = INVALID_SOCKET;
	bool header_sent = false;
	std::vector< uint8_t > packet;
	const int64_t frame_interval_us = 1000000 / std::max< uint32_t >( fps_, 1 );
	int64_t next_frame_us = 0;
	const auto close_client = [ & ]( const char *why ) {
		if ( client != INVALID_SOCKET )
		{
			closesocket( client );
			client = INVALID_SOCKET;
			DriverLog( "desktop layer: client closed (%s)", why );
		}
		header_sent = false;
		streaming_ = false;
	};

	while ( !stop_ )
	{
		if ( client == INVALID_SOCKET )
		{
			SOCKET sock = accept( listen_sock, nullptr, nullptr );
			if ( sock == INVALID_SOCKET )
			{
				Sleep( 50 );
				continue;
			}
			u_long blocking = 0;
			ioctlsocket( sock, FIONBIO, &blocking );
			DWORD timeout_ms = 2000;
			setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast< const char * >( &timeout_ms ), sizeof( timeout_ms ) );
			BOOL no_delay = TRUE;
			setsockopt( sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast< const char * >( &no_delay ), sizeof( no_delay ) );
			client = sock;
			header_sent = false;
			source.RequestKeyframe();
			DriverLog( "desktop layer: client connected" );
		}

		const vr::VROverlayHandle_t panel = panel_;
		if ( panel == vr::k_ulOverlayHandleInvalid )
		{
			if ( ClientClosed( client ) )
			{
				close_client( "disconnected" );
			}
			Sleep( 30 );
			continue;
		}

		const int64_t now_us = SteadyUs();
		if ( now_us < next_frame_us )
		{
			Sleep( static_cast< DWORD >( std::max< int64_t >( 1, ( next_frame_us - now_us ) / 1000 ) ) );
			continue;
		}
		next_frame_us = std::max( next_frame_us + frame_interval_us, now_us );

		switch ( source.Capture( panel, packet, fps_, bitrate_ ) )
		{
		case PanelSource::Result::SizeChanged:
			if ( header_sent )
			{
				close_client( "picture size changed; the Flow reconnects for a new header" );
			}
			continue;
		case PanelSource::Result::None:
			if ( ClientClosed( client ) )
			{
				close_client( "disconnected" );
			}
			continue;
		case PanelSource::Result::Frame:
			break;
		}
		if ( !header_sent )
		{
			if ( !SendHeader( client, packet, source.Width(), source.Height(), fps_ ) )
			{
				source.RequestKeyframe(); // no SPS/PPS in this one: ask again
				continue;
			}
			header_sent = true;
			streaming_ = true;
		}
		if ( !SendFrame( client, packet, SteadyUs() ) )
		{
			close_client( "send failed" );
		}
	}

	close_client( "helper exiting" );
	closesocket( listen_sock );
}
