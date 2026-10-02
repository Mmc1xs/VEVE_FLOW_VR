//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "flow_nvenc_encoder.h"

#include "driverlog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <d3d11.h>
#include <windows.h>

#include "ffnvcodec/nvEncodeAPI.h"

struct FlowNvencEncoder::Api
{
	NV_ENCODE_API_FUNCTION_LIST f{};
};

namespace
{
	using NvEncodeAPICreateInstanceFn = NVENCSTATUS( NVENCAPI * )( NV_ENCODE_API_FUNCTION_LIST * );
	using NvEncodeAPIGetMaxSupportedVersionFn = NVENCSTATUS( NVENCAPI * )( uint32_t * );

	const char *StatusName( NVENCSTATUS status )
	{
		switch ( status )
		{
		case NV_ENC_SUCCESS:
			return "NV_ENC_SUCCESS";
		case NV_ENC_ERR_NO_ENCODE_DEVICE:
			return "NV_ENC_ERR_NO_ENCODE_DEVICE";
		case NV_ENC_ERR_UNSUPPORTED_DEVICE:
			return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
		case NV_ENC_ERR_INVALID_ENCODERDEVICE:
			return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
		case NV_ENC_ERR_INVALID_DEVICE:
			return "NV_ENC_ERR_INVALID_DEVICE";
		case NV_ENC_ERR_DEVICE_NOT_EXIST:
			return "NV_ENC_ERR_DEVICE_NOT_EXIST";
		case NV_ENC_ERR_INVALID_PARAM:
			return "NV_ENC_ERR_INVALID_PARAM";
		case NV_ENC_ERR_INVALID_VERSION:
			return "NV_ENC_ERR_INVALID_VERSION";
		case NV_ENC_ERR_OUT_OF_MEMORY:
			return "NV_ENC_ERR_OUT_OF_MEMORY";
		case NV_ENC_ERR_ENCODER_BUSY:
			return "NV_ENC_ERR_ENCODER_BUSY";
		case NV_ENC_ERR_NEED_MORE_INPUT:
			return "NV_ENC_ERR_NEED_MORE_INPUT";
		default:
			return "NV_ENC_ERR_UNKNOWN";
		}
	}

	uint32_t StructVersionForApi( uint32_t api_version, uint32_t struct_version, bool extended )
	{
		uint32_t version = api_version | ( struct_version << 16 ) | ( 0x7u << 28 );
		if ( extended )
		{
			version |= ( 1u << 31 );
		}
		return version;
	}
}

FlowNvencEncoder::FlowNvencEncoder()
{
	api_ = new Api();
}

FlowNvencEncoder::~FlowNvencEncoder()
{
	Shutdown();
	delete api_;
	api_ = nullptr;
}

void FlowNvencEncoder::SetError( const char *message )
{
	last_error_ = message;
	DriverLog( "Flow NVENC: %s", last_error_.c_str() );
}

void FlowNvencEncoder::SetStatusError( const char *operation, int status )
{
	char buffer[ 256 ];
	std::snprintf( buffer, sizeof( buffer ), "%s failed status=%d %s", operation, status, StatusName( static_cast< NVENCSTATUS >( status ) ) );
	SetError( buffer );
}

bool FlowNvencEncoder::LoadApi()
{
	if ( module_ != nullptr )
	{
		return true;
	}

	HMODULE module = LoadLibraryA( "nvEncodeAPI64.dll" );
	if ( module == nullptr )
	{
		SetError( "LoadLibrary nvEncodeAPI64.dll failed" );
		return false;
	}

	auto create_instance = reinterpret_cast< NvEncodeAPICreateInstanceFn >( GetProcAddress( module, "NvEncodeAPICreateInstance" ) );
	auto get_max_supported_version = reinterpret_cast< NvEncodeAPIGetMaxSupportedVersionFn >( GetProcAddress( module, "NvEncodeAPIGetMaxSupportedVersion" ) );
	if ( create_instance == nullptr )
	{
		FreeLibrary( module );
		SetError( "GetProcAddress NvEncodeAPICreateInstance failed" );
		return false;
	}
	api_version_ = 11;

	std::memset( &api_->f, 0, sizeof( api_->f ) );
	api_->f.version = StructVersionForApi( api_version_, 2, false );
	const NVENCSTATUS status = create_instance( &api_->f );
	if ( status != NV_ENC_SUCCESS )
	{
		FreeLibrary( module );
		SetStatusError( "NvEncodeAPICreateInstance", status );
		return false;
	}

	module_ = module;
	return true;
}

bool FlowNvencEncoder::Initialize( ID3D11Device *device, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, uint32_t preset )
{
	Shutdown();
	width_ = width;
	height_ = height;
	fps_ = fps;
	frame_index_ = 0;
	sent_headers_ = false;

	if ( device == nullptr )
	{
		SetError( "Initialize called with null D3D11 device" );
		return false;
	}
	if ( !LoadApi() )
	{
		return false;
	}

	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params{};
	session_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	session_params.version = StructVersionForApi( api_version_, 1, false );
	session_params.device = device;
	session_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	session_params.apiVersion = api_version_;
	DriverLog( "Flow NVENC: opening session api=0x%08x sessionVersion=0x%08x", api_version_, session_params.version );
	NVENCSTATUS status = api_->f.nvEncOpenEncodeSessionEx( &session_params, &encoder_ );
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncOpenEncodeSessionEx", status );
		Shutdown();
		return false;
	}

	static const GUID kPresets[] = { NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_PRESET_P4_GUID,
	                                 NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID };
	const GUID preset_guid = kPresets[ std::clamp< uint32_t >( preset, 1, 7 ) - 1 ];

	NV_ENC_PRESET_CONFIG preset_config{};
	preset_config.version = StructVersionForApi( api_version_, 5, true );
	preset_config.presetCfg.version = StructVersionForApi( api_version_, 9, true );
	status = api_->f.nvEncGetEncodePresetConfigEx( encoder_, NV_ENC_CODEC_H264_GUID, preset_guid,
	                                               NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_config );
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncGetEncodePresetConfigEx", status );
		Shutdown();
		return false;
	}

	NV_ENC_CONFIG config = preset_config.presetCfg;
	config.version = StructVersionForApi( api_version_, 9, true );
	config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
	config.gopLength = fps;
	config.frameIntervalP = 1;
	config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
	config.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
	config.rcParams.version = StructVersionForApi( api_version_, 1, false );
	config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	config.rcParams.averageBitRate = bitrate;
	config.rcParams.maxBitRate = bitrate;
	config.rcParams.vbvBufferSize = std::max< uint32_t >( bitrate / std::max< uint32_t >( fps, 1 ), 256 * 1024 );
	config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
	config.rcParams.enableLookahead = 0;
	config.rcParams.enableAQ = 0;
	config.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;
	config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
	config.encodeCodecConfig.h264Config.idrPeriod = fps;
	config.encodeCodecConfig.h264Config.sliceMode = 0;
	config.encodeCodecConfig.h264Config.sliceModeData = 0;
	config.encodeCodecConfig.h264Config.disableSPSPPS = 0;

	NV_ENC_INITIALIZE_PARAMS init{};
	init.version = StructVersionForApi( api_version_, 7, true );
	init.encodeGUID = NV_ENC_CODEC_H264_GUID;
	init.presetGUID = preset_guid;
	init.encodeWidth = width;
	init.encodeHeight = height;
	init.darWidth = width;
	init.darHeight = height;
	init.frameRateNum = fps;
	init.frameRateDen = 1;
	init.enableEncodeAsync = 0;
	init.enablePTD = 1;
	init.encodeConfig = &config;
	init.maxEncodeWidth = width;
	init.maxEncodeHeight = height;
	init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

	status = api_->f.nvEncInitializeEncoder( encoder_, &init );
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncInitializeEncoder", status );
		Shutdown();
		return false;
	}

	NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
	bitstream.version = StructVersionForApi( api_version_, 1, false );
	status = api_->f.nvEncCreateBitstreamBuffer( encoder_, &bitstream );
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncCreateBitstreamBuffer", status );
		Shutdown();
		return false;
	}
	bitstream_buffer_ = bitstream.bitstreamBuffer;

	char line[ 192 ];
	std::snprintf( line, sizeof( line ), "Flow NVENC initialized H264 %ux%u@%u bitrate=%u preset=P%u api=0x%08x",
	               width, height, fps, bitrate, std::clamp< uint32_t >( preset, 1, 7 ), api_version_ );
	DriverLog( "%s", line );
	last_error_.clear();
	return true;
}

void FlowNvencEncoder::UnregisterInput()
{
	if ( encoder_ != nullptr )
	{
		for ( const auto &registration : registrations_ )
		{
			api_->f.nvEncUnregisterResource( encoder_, registration.second );
		}
	}
	registrations_.clear();
}

bool FlowNvencEncoder::EncodeTexture( ID3D11Texture2D *texture, uint64_t pts_us, std::vector< uint8_t > &out_packet,
                                      std::mutex *device_mutex )
{
	out_packet.clear();
	if ( encoder_ == nullptr || bitstream_buffer_ == nullptr || texture == nullptr )
	{
		return false;
	}

	std::unique_lock< std::mutex > device_lock;
	if ( device_mutex != nullptr )
	{
		device_lock = std::unique_lock< std::mutex >( *device_mutex );
	}

	void *registered_resource = nullptr;
	for ( const auto &registration : registrations_ )
	{
		if ( registration.first == texture )
		{
			registered_resource = registration.second;
		}
	}
	if ( registered_resource == nullptr )
	{
		NV_ENC_REGISTER_RESOURCE reg{};
		reg.version = StructVersionForApi( api_version_, 5, false );
		reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
		reg.width = width_;
		reg.height = height_;
		reg.pitch = 0;
		reg.subResourceIndex = 0;
		reg.resourceToRegister = texture;
		reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
		reg.bufferUsage = NV_ENC_INPUT_IMAGE;
		const NVENCSTATUS status = api_->f.nvEncRegisterResource( encoder_, &reg );
		if ( status != NV_ENC_SUCCESS )
		{
			SetStatusError( "nvEncRegisterResource", status );
			return false;
		}
		registered_resource = reg.registeredResource;
		registrations_.emplace_back( texture, registered_resource );
	}

	NV_ENC_MAP_INPUT_RESOURCE map{};
	map.version = StructVersionForApi( api_version_, 4, false );
	map.registeredResource = registered_resource;
	NVENCSTATUS status = api_->f.nvEncMapInputResource( encoder_, &map );
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncMapInputResource", status );
		return false;
	}

	NV_ENC_PIC_PARAMS pic{};
	pic.version = StructVersionForApi( api_version_, 7, true );
	pic.inputWidth = width_;
	pic.inputHeight = height_;
	pic.inputPitch = width_;
	pic.encodePicFlags = sent_headers_ ? 0 : ( NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS );
	pic.frameIdx = frame_index_++;
	pic.inputTimeStamp = pts_us;
	pic.inputDuration = 1000000ull / std::max< uint32_t >( fps_, 1 );
	pic.inputBuffer = map.mappedResource;
	pic.outputBitstream = bitstream_buffer_;
	pic.bufferFmt = map.mappedBufferFmt;
	pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

	status = api_->f.nvEncEncodePicture( encoder_, &pic );
	api_->f.nvEncUnmapInputResource( encoder_, map.mappedResource );
	if ( device_lock.owns_lock() )
	{
		device_lock.unlock(); // the bitstream wait below needs no D3D access
	}
	if ( status == NV_ENC_ERR_NEED_MORE_INPUT )
	{
		return true;
	}
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncEncodePicture", status );
		return false;
	}

	NV_ENC_LOCK_BITSTREAM lock{};
	lock.version = StructVersionForApi( api_version_, 2, true );
	lock.outputBitstream = bitstream_buffer_;
	lock.doNotWait = 0;
	status = api_->f.nvEncLockBitstream( encoder_, &lock );
	if ( status != NV_ENC_SUCCESS )
	{
		SetStatusError( "nvEncLockBitstream", status );
		return false;
	}

	const auto *bytes = static_cast< const uint8_t * >( lock.bitstreamBufferPtr );
	out_packet.assign( bytes, bytes + lock.bitstreamSizeInBytes );
	api_->f.nvEncUnlockBitstream( encoder_, bitstream_buffer_ );
	sent_headers_ = true;
	return true;
}

void FlowNvencEncoder::Shutdown()
{
	UnregisterInput();
	if ( encoder_ != nullptr && bitstream_buffer_ != nullptr )
	{
		api_->f.nvEncDestroyBitstreamBuffer( encoder_, bitstream_buffer_ );
	}
	bitstream_buffer_ = nullptr;
	if ( encoder_ != nullptr )
	{
		api_->f.nvEncDestroyEncoder( encoder_ );
	}
	encoder_ = nullptr;
	if ( module_ != nullptr )
	{
		FreeLibrary( static_cast< HMODULE >( module_ ) );
	}
	module_ = nullptr;
}
