#pragma once

#include <atomic>
#include <thread>

// Captures whatever the PC plays (WASAPI loopback of the Windows default output device, so the
// PC speakers keep playing too) and streams it to the Flow over TCP 8004.
//
// Stream: "FLOWAUD1", int32 version, int32 sample rate, int32 channels (2) (big-endian),
// then raw interleaved signed 16-bit little-endian PCM until the socket closes.
// When the default device changes to a different sample rate the client is dropped and
// the Flow reconnects to read the new header.
class FlowAudioStreamer
{
public:
	FlowAudioStreamer();
	~FlowAudioStreamer();

private:
	void Run();

	std::atomic< bool > stop_{ false };
	std::thread thread_;
};
