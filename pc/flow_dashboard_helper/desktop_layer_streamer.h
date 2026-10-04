#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

// Streams Desktop+'s own panel picture to the Flow on TCP 8005, where it is shown as a Wave
// compositor layer exactly over the Desktop+ panel: sampled once there, so text stays sharp,
// while Desktop+ still handles the laser and its tools. The picture is the part of Desktop+'s
// overlay texture the panel shows (read through IVROverlay::GetOverlayTexture), so whatever
// Desktop+ displays (monitor, crop, its cursor) is what the Flow shows.
//
// Stream: "FLOWH264", version 4, width, height, fps, layout 0 (big-endian u32), length-prefixed
// SPS and PPS, then per VCL NAL: u32 size, i64 pts_us, i64 encoded_ms, i64 send_start_ms, the NAL,
// i64 send_end_ms. The Flow reconnects for a new header when the picture size changes.
class DesktopLayerStreamer
{
public:
	DesktopLayerStreamer( uint32_t bitrate, uint32_t fps );
	~DesktopLayerStreamer();

	// Set every loop by the helper: the Desktop+ overlay the dashboard shows now (each Desktop+
	// overlay is its own panel, e.g. one per monitor), or k_ulOverlayHandleInvalid for none.
	void SetPanel( uint64_t overlay_handle ) { panel_ = overlay_handle; }
	// The Flow is receiving the picture (it then covers the panel).
	bool Streaming() const { return streaming_; }

private:
	void Run();

	uint32_t bitrate_;
	uint32_t fps_;
	std::atomic< bool > stop_{ false };
	std::atomic< uint64_t > panel_{ 0 };
	std::atomic< bool > streaming_{ false };
	std::thread thread_;
};
