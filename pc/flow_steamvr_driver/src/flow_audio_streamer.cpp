#include "flow_audio_streamer.h"

#include "driverlog.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr uint16_t kAudioPort = 8004;
	constexpr int32_t kAudioProtocolVersion = 1;
	constexpr int32_t kAudioChannels = 2;
	// Shared-mode loopback buffer; we poll it every few ms.
	constexpr REFERENCE_TIME kCaptureBufferDuration = 1000000; // 100 ms in 100 ns units
	constexpr auto kDeviceCheckInterval = std::chrono::seconds( 1 );
	constexpr auto kClientCheckInterval = std::chrono::milliseconds( 200 );

	// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT without pulling in ksmedia.h / ksuser.lib.
	constexpr GUID kSubtypeIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

	template < typename T >
	void SafeRelease( T *&p )
	{
		if ( p != nullptr )
		{
			p->Release();
			p = nullptr;
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

	// The Flow never sends anything on this socket, so "readable" means it closed.
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

	std::wstring DefaultRenderDeviceId( IMMDeviceEnumerator *enumerator )
	{
		IMMDevice *device = nullptr;
		if ( FAILED( enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device ) ) )
		{
			return std::wstring();
		}
		std::wstring id;
		LPWSTR raw_id = nullptr;
		if ( SUCCEEDED( device->GetId( &raw_id ) ) )
		{
			id = raw_id;
			CoTaskMemFree( raw_id );
		}
		device->Release();
		return id;
	}

	// Loopback capture of the default render device, converted to 16-bit stereo.
	class LoopbackCapture
	{
	public:
		~LoopbackCapture() { Close(); }

		bool Open( IMMDeviceEnumerator *enumerator )
		{
			Close();
			IMMDevice *device = nullptr;
			HRESULT hr = enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device );
			if ( FAILED( hr ) )
			{
				DriverLog( "Flow audio: no default output device (0x%08lx)", hr );
				return false;
			}
			LPWSTR raw_id = nullptr;
			if ( SUCCEEDED( device->GetId( &raw_id ) ) )
			{
				device_id_ = raw_id;
				CoTaskMemFree( raw_id );
			}
			hr = device->Activate( __uuidof( IAudioClient ), CLSCTX_ALL, nullptr, reinterpret_cast< void ** >( &client_ ) );
			device->Release();
			if ( FAILED( hr ) )
			{
				DriverLog( "Flow audio: Activate failed (0x%08lx)", hr );
				return false;
			}

			WAVEFORMATEX *format = nullptr;
			hr = client_->GetMixFormat( &format );
			if ( FAILED( hr ) )
			{
				DriverLog( "Flow audio: GetMixFormat failed (0x%08lx)", hr );
				Close();
				return false;
			}
			sample_rate_ = static_cast< int32_t >( format->nSamplesPerSec );
			channels_ = format->nChannels;
			bits_ = format->wBitsPerSample;
			block_align_ = format->nBlockAlign;
			is_float_ = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
			            ( format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
			              IsEqualGUID( reinterpret_cast< WAVEFORMATEXTENSIBLE * >( format )->SubFormat, kSubtypeIeeeFloat ) );

			hr = client_->Initialize( AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kCaptureBufferDuration, 0, format, nullptr );
			CoTaskMemFree( format );
			if ( FAILED( hr ) )
			{
				DriverLog( "Flow audio: loopback Initialize failed (0x%08lx)", hr );
				Close();
				return false;
			}
			if ( ( is_float_ && bits_ != 32 ) || ( !is_float_ && bits_ != 16 && bits_ != 24 && bits_ != 32 ) )
			{
				DriverLog( "Flow audio: unsupported mix format float=%d bits=%u", is_float_ ? 1 : 0, bits_ );
				Close();
				return false;
			}
			hr = client_->GetService( __uuidof( IAudioCaptureClient ), reinterpret_cast< void ** >( &capture_ ) );
			if ( FAILED( hr ) || FAILED( client_->Start() ) )
			{
				DriverLog( "Flow audio: capture start failed (0x%08lx)", hr );
				Close();
				return false;
			}
			DriverLog( "Flow audio: capturing default output %d Hz, %u ch, %u-bit %s",
			           sample_rate_, channels_, bits_, is_float_ ? "float" : "int" );
			return true;
		}

		void Close()
		{
			if ( client_ != nullptr )
			{
				client_->Stop();
			}
			SafeRelease( capture_ );
			SafeRelease( client_ );
		}

		// Appends every pending packet to out. Returns false when the device went away.
		bool Read( std::vector< int16_t > &out )
		{
			for ( ;; )
			{
				UINT32 packet_frames = 0;
				HRESULT hr = capture_->GetNextPacketSize( &packet_frames );
				if ( FAILED( hr ) )
				{
					return false;
				}
				if ( packet_frames == 0 )
				{
					return true;
				}
				BYTE *data = nullptr;
				UINT32 frames = 0;
				DWORD flags = 0;
				hr = capture_->GetBuffer( &data, &frames, &flags, nullptr, nullptr );
				if ( FAILED( hr ) )
				{
					return false;
				}
				const size_t base = out.size();
				out.resize( base + static_cast< size_t >( frames ) * kAudioChannels );
				int16_t *dst = out.data() + base;
				if ( ( flags & AUDCLNT_BUFFERFLAGS_SILENT ) != 0 )
				{
					std::memset( dst, 0, static_cast< size_t >( frames ) * kAudioChannels * sizeof( int16_t ) );
				}
				else
				{
					for ( UINT32 i = 0; i < frames; ++i )
					{
						const BYTE *frame = data + static_cast< size_t >( i ) * block_align_;
						float left = Sample( frame, 0 );
						float right = channels_ >= 2 ? Sample( frame, 1 ) : left;
						// Fold surround layouts (FL FR FC LFE BL BR [SL SR]) into stereo.
						if ( channels_ >= 3 )
						{
							const float center = 0.707f * Sample( frame, 2 );
							left += center;
							right += center;
						}
						for ( unsigned ch = 4; ch + 1 < channels_; ch += 2 )
						{
							left += 0.707f * Sample( frame, ch );
							right += 0.707f * Sample( frame, ch + 1 );
						}
						dst[ i * 2 ] = ToInt16( left );
						dst[ i * 2 + 1 ] = ToInt16( right );
					}
				}
				capture_->ReleaseBuffer( frames );
			}
		}

		const std::wstring &DeviceId() const { return device_id_; }
		int32_t SampleRate() const { return sample_rate_; }

	private:
		float Sample( const BYTE *frame, unsigned channel ) const
		{
			const BYTE *p = frame + channel * ( bits_ / 8 );
			if ( is_float_ )
			{
				float value;
				std::memcpy( &value, p, sizeof( value ) );
				return value;
			}
			switch ( bits_ )
			{
			case 16:
			{
				int16_t value;
				std::memcpy( &value, p, sizeof( value ) );
				return value / 32768.0f;
			}
			case 24:
			{
				const int32_t value = static_cast< int32_t >( ( static_cast< uint32_t >( p[ 0 ] ) << 8 ) |
				                                              ( static_cast< uint32_t >( p[ 1 ] ) << 16 ) |
				                                              ( static_cast< uint32_t >( p[ 2 ] ) << 24 ) );
				return value / 2147483648.0f;
			}
			default:
			{
				int32_t value;
				std::memcpy( &value, p, sizeof( value ) );
				return value / 2147483648.0f;
			}
			}
		}

		static int16_t ToInt16( float value )
		{
			value = std::clamp( value, -1.0f, 1.0f );
			return static_cast< int16_t >( value * 32767.0f );
		}

		IAudioClient *client_ = nullptr;
		IAudioCaptureClient *capture_ = nullptr;
		std::wstring device_id_;
		int32_t sample_rate_ = 0;
		unsigned channels_ = 0;
		unsigned bits_ = 0;
		unsigned block_align_ = 0;
		bool is_float_ = false;
	};

	bool SendHeader( SOCKET sock, int32_t sample_rate )
	{
		char header[ 8 + 3 * 4 ];
		std::memcpy( header, "FLOWAUD1", 8 );
		const int32_t fields[ 3 ] = { kAudioProtocolVersion, sample_rate, kAudioChannels };
		for ( int i = 0; i < 3; ++i )
		{
			const u_long be = htonl( static_cast< u_long >( fields[ i ] ) );
			std::memcpy( header + 8 + i * 4, &be, 4 );
		}
		return SendAll( sock, header, sizeof( header ) );
	}

	// Streams until the client disconnects, the sample rate changes, or stop is set.
	void StreamToClient( SOCKET sock, IMMDeviceEnumerator *enumerator, const std::atomic< bool > &stop )
	{
		LoopbackCapture capture;
		if ( !capture.Open( enumerator ) || !SendHeader( sock, capture.SampleRate() ) )
		{
			return;
		}
		const int32_t sample_rate = capture.SampleRate();
		std::vector< int16_t > pcm;
		uint64_t sent_frames = 0;
		auto next_device_check = std::chrono::steady_clock::now() + kDeviceCheckInterval;
		auto next_client_check = std::chrono::steady_clock::now() + kClientCheckInterval;
		auto next_log = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );

		while ( !stop )
		{
			const auto now = std::chrono::steady_clock::now();
			bool reopen = false;
			if ( now >= next_device_check )
			{
				next_device_check = now + kDeviceCheckInterval;
				reopen = DefaultRenderDeviceId( enumerator ) != capture.DeviceId();
			}

			pcm.clear();
			if ( !capture.Read( pcm ) )
			{
				reopen = true;
			}
			if ( reopen )
			{
				DriverLog( "Flow audio: default output device changed, reopening" );
				if ( !capture.Open( enumerator ) || capture.SampleRate() != sample_rate )
				{
					return; // the Flow reconnects and reads a fresh header
				}
				continue;
			}

			if ( !pcm.empty() )
			{
				if ( !SendAll( sock, pcm.data(), pcm.size() * sizeof( int16_t ) ) )
				{
					DriverLog( "Flow audio: send failed (wsa=%d)", WSAGetLastError() );
					return;
				}
				sent_frames += pcm.size() / kAudioChannels;
			}
			else
			{
				if ( now >= next_client_check )
				{
					next_client_check = now + kClientCheckInterval;
					if ( ClientClosed( sock ) )
					{
						DriverLog( "Flow audio: client closed" );
						return;
					}
				}
				Sleep( 5 );
			}

			if ( now >= next_log )
			{
				next_log = now + std::chrono::seconds( 10 );
				DriverLog( "Flow audio: %.1f s sent", static_cast< double >( sent_frames ) / sample_rate );
			}
		}
	}
} // namespace

FlowAudioStreamer::FlowAudioStreamer()
{
	thread_ = std::thread( &FlowAudioStreamer::Run, this );
}

FlowAudioStreamer::~FlowAudioStreamer()
{
	stop_ = true;
	if ( thread_.joinable() )
	{
		thread_.join();
	}
}

void FlowAudioStreamer::Run()
{
	const HRESULT com_hr = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
	WSADATA wsa_data{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) != 0 )
	{
		DriverLog( "Flow audio: WSAStartup failed" );
		return;
	}

	IMMDeviceEnumerator *enumerator = nullptr;
	SOCKET listen_sock = INVALID_SOCKET;
	if ( FAILED( CoCreateInstance( __uuidof( MMDeviceEnumerator ), nullptr, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ),
	                               reinterpret_cast< void ** >( &enumerator ) ) ) )
	{
		DriverLog( "Flow audio: MMDeviceEnumerator unavailable" );
	}
	else
	{
		listen_sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
		BOOL reuse = TRUE;
		setsockopt( listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast< const char * >( &reuse ), sizeof( reuse ) );
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons( kAudioPort );
		address.sin_addr.s_addr = htonl( INADDR_ANY );
		if ( listen_sock == INVALID_SOCKET ||
		     bind( listen_sock, reinterpret_cast< sockaddr * >( &address ), sizeof( address ) ) == SOCKET_ERROR ||
		     listen( listen_sock, 1 ) == SOCKET_ERROR )
		{
			DriverLog( "Flow audio: listen on 0.0.0.0:%u failed (wsa=%d)", kAudioPort, WSAGetLastError() );
			if ( listen_sock != INVALID_SOCKET )
			{
				closesocket( listen_sock );
				listen_sock = INVALID_SOCKET;
			}
		}
		else
		{
			u_long non_blocking = 1;
			ioctlsocket( listen_sock, FIONBIO, &non_blocking );
			DriverLog( "Flow audio: listening on 0.0.0.0:%u", kAudioPort );
		}
	}

	while ( !stop_ && listen_sock != INVALID_SOCKET )
	{
		SOCKET sock = accept( listen_sock, nullptr, nullptr );
		if ( sock == INVALID_SOCKET )
		{
			Sleep( 200 );
			continue;
		}
		u_long blocking = 0;
		ioctlsocket( sock, FIONBIO, &blocking );
		DWORD timeout_ms = 1000;
		setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast< const char * >( &timeout_ms ), sizeof( timeout_ms ) );
		BOOL no_delay = TRUE;
		setsockopt( sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast< const char * >( &no_delay ), sizeof( no_delay ) );
		DriverLog( "Flow audio: client connected" );

		StreamToClient( sock, enumerator, stop_ );
		closesocket( sock );
		DriverLog( "Flow audio: client disconnected" );
	}

	if ( listen_sock != INVALID_SOCKET )
	{
		closesocket( listen_sock );
	}
	SafeRelease( enumerator );
	WSACleanup();
	if ( SUCCEEDED( com_hr ) )
	{
		CoUninitialize();
	}
}
