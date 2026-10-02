//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

class FlowNvencEncoder
{
public:
	FlowNvencEncoder();
	~FlowNvencEncoder();

	// preset: NVENC P1 (fastest) .. P7 (best quality); always with ultra-low-latency tuning.
	bool Initialize( ID3D11Device *device, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, uint32_t preset );
	// device_mutex (optional) is held only while submitting work to the D3D device, not while
	// waiting for the bitstream, so a render thread sharing the device is never blocked on NVENC.
	bool EncodeTexture( ID3D11Texture2D *texture, uint64_t pts_us, std::vector< uint8_t > &out_packet,
	                    std::mutex *device_mutex = nullptr );
	// Next frame becomes an IDR with SPS/PPS, e.g. so a newly connected client can start decoding.
	void RequestKeyframe() { sent_headers_ = false; }
	void Shutdown();

	const std::string &LastError() const { return last_error_; }
	bool IsInitialized() const { return encoder_ != nullptr; }

private:
	bool LoadApi();
	void SetError( const char *message );
	void SetStatusError( const char *operation, int status );
	void UnregisterInput();

	void *module_ = nullptr;
	void *encoder_ = nullptr;
	void *bitstream_buffer_ = nullptr;
	// Input textures registered with NVENC; callers rotate through a few, so keep them all.
	std::vector< std::pair< ID3D11Texture2D *, void * > > registrations_;

	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t fps_ = 0;
	uint32_t api_version_ = 0;
	uint32_t frame_index_ = 0;
	bool sent_headers_ = false;
	std::string last_error_;
	std::vector< uint8_t > packet_buffer_;

	struct Api;
	Api *api_ = nullptr;
};
