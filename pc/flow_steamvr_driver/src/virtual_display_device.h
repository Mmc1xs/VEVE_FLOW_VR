//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include "openvr_driver.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
using FlowSocketHandle = uintptr_t;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;
struct IDXGIFactory1;
#endif

class FlowNvencEncoder;

class FlowVirtualDisplayDevice : public vr::ITrackedDeviceServerDriver, public vr::IVRVirtualDisplay
{
public:
	FlowVirtualDisplayDevice();
	~FlowVirtualDisplayDevice();

	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void Deactivate() override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;

	void Present( const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize ) override;
	void WaitForPresent() override;
	bool GetTimeSinceLastVsync( float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter ) override;

	const std::string &MyGetSerialNumber() const;

private:
	bool InitializeD3DResources();
	ID3D11Texture2D *OpenSharedTexture( vr::SharedTextureHandle_t texture_handle );
	void ProbeSharedTexture( const vr::PresentInfo_t &present_info, uint64_t present_count );
	void DumpTexturePreview( ID3D11Texture2D *texture, const vr::PresentInfo_t &present_info );
	void FinishTexturePreviewDump();
	// Diagnostics (encode thread): logs\dump_stream.request records the next second of the stream.
	void CheckStreamDumpRequest();
	void RecordStreamDumpPacket( int slot, const std::vector< uint8_t > &packet );
	void WriteEncoderInputPpm( int slot );
	void StreamTexturePreview( ID3D11Texture2D *texture, const vr::PresentInfo_t &present_info, uint64_t present_count );
	bool EnsureStreamDownsampleResources( uint32_t source_format );
	bool RenderStreamDownsample( ID3D11Texture2D *texture, int slot );
	bool EnsureStreamSocketConnected();
	bool EnsureH264StreamHeader( const std::vector< uint8_t > &packet );
	bool SendH264Packet( const std::vector< uint8_t > &packet, uint64_t pts_us, uint64_t encoded_ready_ms, uint32_t pose_sequence );
	bool SendStreamBytes( const void *data, size_t size );
	void BroadcastDiscoveryIfDue();
	void LogStreamStatsIfDue( std::chrono::steady_clock::time_point now );
	void StartEncodeThread();
	void StopEncodeThread();
	void SetStreamClientConnected( bool connected );
	void EncodeThreadMain();
	void EncodeAndSendSlot( int slot, uint64_t pts_us, uint32_t pose_sequence );
	void CloseStreamSocket();
	void ReleaseD3DResources();

	uint32_t device_index_ = vr::k_unTrackedDeviceIndexInvalid;
	std::string serial_number_;
	std::atomic< bool > is_active_{ false };
	std::atomic< uint64_t > present_count_{ 0 };
	std::atomic< uint64_t > vsync_counter_{ 0 };
	uint64_t graphics_adapter_luid_ = 0;
	std::chrono::steady_clock::time_point start_time_;
	std::chrono::steady_clock::time_point last_vsync_;

#ifdef _WIN32
	std::mutex d3d_mutex_;
	IDXGIFactory1 *dxgi_factory_ = nullptr;
	ID3D11Device *d3d_device_ = nullptr;
	ID3D11DeviceContext *d3d_context_ = nullptr;
	ID3D11Texture2D *flush_texture_ = nullptr;
	ID3D11Texture2D *dump_texture_ = nullptr;
	// Downsampled frames waiting for / being encoded. Present renders into a free slot while
	// the encode thread works on another, so NVENC never runs on the compositor's thread.
	static constexpr int kStreamSlotCount = 3;
	ID3D11Texture2D *stream_slot_textures_[ kStreamSlotCount ] = {};
	ID3D11RenderTargetView *stream_slot_rtvs_[ kStreamSlotCount ] = {};
	ID3D11VertexShader *stream_vertex_shader_ = nullptr;
	ID3D11PixelShader *stream_pixel_shader_ = nullptr;
	ID3D11SamplerState *stream_sampler_ = nullptr;
	uint32_t stream_texture_format_ = 0;
	std::unordered_map< uint64_t, ID3D11Texture2D * > shared_textures_;
	std::vector< uint8_t > stream_buffer_;
	std::unique_ptr< FlowNvencEncoder > nvenc_encoder_;
	// Side-by-side stereo stream size; read from flowvr_display settings.
	uint32_t stream_width_ = 1920;
	uint32_t stream_height_ = 960;
	// Encoder quality; read from flowvr_display settings.
	uint32_t stream_bitrate_ = 100000000;
	uint32_t nvenc_preset_ = 4;
	FlowSocketHandle stream_socket_ = ~static_cast< FlowSocketHandle >( 0 );
	FlowSocketHandle stream_listen_socket_ = ~static_cast< FlowSocketHandle >( 0 );
	FlowSocketHandle stream_discovery_socket_ = ~static_cast< FlowSocketHandle >( 0 );
	std::chrono::steady_clock::time_point last_discovery_broadcast_;
	bool logged_discovery_targets_ = false;
	bool stream_wsa_started_ = false;
	bool stream_header_sent_ = false;
	bool owns_stream_ = false;
	bool nvenc_failed_ = false;
	bool logged_texture_desc_ = false;
	bool dumped_texture_preview_ = false;
	// Preview copy queued on the GPU, read back on a later Present once it has finished.
	bool dump_readback_pending_ = false;
	uint64_t dump_frame_id_ = 0;
	// Stream dump: an IDR plus the following frames as raw H.264, and the encoder input of the
	// last one as a full-size PPM, so the decoded last frame can be compared pixel for pixel.
	FILE *stream_dump_file_ = nullptr;
	int stream_dump_frames_left_ = 0;
	std::chrono::steady_clock::time_point next_stream_dump_check_;
	std::chrono::steady_clock::time_point last_stream_connect_attempt_;
	std::chrono::steady_clock::time_point next_stream_frame_;

	// Present -> encode thread hand-off. A newer frame replaces one not yet picked up.
	std::thread encode_thread_;
	std::mutex encode_mutex_;
	std::condition_variable encode_cv_;
	bool encode_stop_ = false;
	int pending_slot_ = -1;
	int encoding_slot_ = -1;
	uint64_t pending_pts_us_ = 0;
	uint32_t pending_pose_sequence_ = 0;
	std::atomic< bool > stream_client_connected_{ false };

	// Present-thread counters (read and reset by the encode thread's stats log).
	std::atomic< uint32_t > stats_calls_{ 0 };
	std::atomic< uint32_t > stats_throttled_{ 0 };
	std::atomic< uint32_t > stats_captured_{ 0 };
	std::atomic< uint32_t > stats_replaced_{ 0 };
	std::atomic< uint64_t > stats_capture_us_{ 0 };
	// Encode-thread counters.
	std::chrono::steady_clock::time_point stats_window_start_;
	uint64_t stats_present_base_ = 0;
	uint32_t stats_sent_ = 0;
	double stats_encode_ms_ = 0.0;
	double stats_encode_max_ms_ = 0.0;
	double stats_send_ms_ = 0.0;
#endif
};
