package com.htc.vr.samples.wvr_flow_probe;

import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.graphics.SurfaceTexture;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

import com.htc.vr.sdk.VRActivity;

import java.lang.reflect.Method;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;

public class MainActivity extends VRActivity {
    private static final String TAG = "FlowProbe";
    private static final String TEST_VIDEO_ASSET = "decoder_test.mp4";
    private static final String TEST_RAW_H264_ASSET = "decoder_test.h264";
    private static final String TEST_VIDEO_URL = "http://127.0.0.1:8000/decoder_test.mp4";
    private static final String[] SOCKET_HOSTS = {
            "192.168.0.102",
            "127.0.0.1"
    };
    private static final int SOCKET_PORT = 8001;
    private static final int SOCKET_DISCOVERY_PORT = 8002;
    private static final int SOCKET_DISCOVERY_TIMEOUT_MS = 5000;
    private static final int SOCKET_CONNECT_TIMEOUT_MS = 1500;
    private static final int SOCKET_READ_TIMEOUT_MS = 15000;
    private static final String SOCKET_MAGIC = "FLOWH264";
    private static final String SOCKET_DISCOVERY_MAGIC = "FLOWH264_PC";
    // PC audio (driver FlowAudioStreamer): same host as the video stream.
    private static final int AUDIO_PORT = 8004;
    private static final String AUDIO_MAGIC = "FLOWAUD1";
    private static final int AUDIO_CHUNK_MS = 10;
    private static final int AUDIO_TRACK_BUFFER_MS = 120;
    // Queue cap: chunks beyond this are dropped so Wi-Fi bursts and clock drift can't pile up latency.
    private static final int AUDIO_MAX_QUEUED_MS = 60;
    // Silence written ahead of the first chunk after the queue ran dry, as a jitter cushion.
    private static final int AUDIO_REFILL_PAD_MS = 20;
    // Desktop layer stream (FLOWH264 from the PC desktop sender): shown by the native renderer
    // as a Wave compositor layer. Same host as the video stream.
    private static final int DESKTOP_PORT = 8005;
    private static final int STREAM_LAYOUT_MONO = 0;
    private static final boolean USE_SOCKET_H264_FEEDER = true;
    private static final boolean USE_RAW_H264_FEEDER = true;
    private static final String DECODER_MODE_PROP = "debug.flow.decoder.mode";
    private static final String DECODER_MODE_BASELINE = "baseline";
    private static final String DECODER_MODE_LOW_LATENCY = "lowlat";
    private static final String KEY_LOW_LATENCY = "low-latency";
    private static final String KEY_VENDOR_QTI_LOW_LATENCY = "vendor.qti-ext-dec-low-latency.enable";

    private DecoderThread mDecoderThread;
    // GL_TEXTURE_EXTERNAL_OES name owned by the native renderer; 0 until initGL runs.
    private volatile int mDecoderTextureName;
    private SurfaceTexture mDecoderTexture;
    private Surface mDecoderSurface;
    // Desktop layer: texture owned by the native renderer, surface used by DesktopThread.
    private volatile int mDesktopTextureName;
    private SurfaceTexture mDesktopTexture;
    private static volatile Surface sDesktopSurface;

    static {
        System.loadLibrary("hellovr_jni");
    }

    @Override
    protected void onCreate(Bundle state) {
        Log.i(TAG, "onCreate: native init");
        init(getResources().getAssets());
        super.onCreate(state);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
        // onPause tears the decoder down (e.g. headset taken off); bring it back once the
        // native renderer has handed us its texture. The first start comes from native initGL.
        if (mDesktopTextureName != 0 && mDesktopTexture == null) {
            startDesktopSurface(mDesktopTextureName);
        }
        if (mDecoderTextureName != 0 && mDecoderThread == null) {
            startDecoderSurface(mDecoderTextureName);
        }
    }

    @Override
    protected void onPause() {
        Log.i(TAG, "onPause");
        stopDecoder();
        stopDesktopSurface();
        super.onPause();
    }

    @SuppressWarnings("unused")
    public native void init(AssetManager assetManager);

    @SuppressWarnings("unused")
    public synchronized void startDecoderSurface(int textureName) {
        Log.i(TAG, "startDecoderSurface texture=" + textureName);
        stopDecoder();
        mDecoderTextureName = textureName;

        mDecoderTexture = new SurfaceTexture(textureName);
        mDecoderTexture.setDefaultBufferSize(1280, 720);
        mDecoderSurface = new Surface(mDecoderTexture);
        setDecoderSurfaceTexture(mDecoderTexture);

        mDecoderThread = new DecoderThread(getAssets(), mDecoderSurface);
        mDecoderThread.start();
    }

    private synchronized void stopDecoder() {
        if (mDecoderThread != null) {
            mDecoderThread.requestStop();
            try {
                mDecoderThread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            mDecoderThread = null;
        }
        clearDecoderSurfaceTexture();
        if (mDecoderSurface != null) {
            mDecoderSurface.release();
            mDecoderSurface = null;
        }
        if (mDecoderTexture != null) {
            mDecoderTexture.release();
            mDecoderTexture = null;
        }
    }

    @SuppressWarnings("unused")
    public synchronized void startDesktopSurface(int textureName) {
        Log.i(TAG, "startDesktopSurface texture=" + textureName);
        stopDesktopSurface();
        mDesktopTextureName = textureName;
        mDesktopTexture = new SurfaceTexture(textureName);
        mDesktopTexture.setDefaultBufferSize(1920, 1080);
        sDesktopSurface = new Surface(mDesktopTexture);
        setDesktopSurfaceTexture(mDesktopTexture);
    }

    // Called after stopDecoder, which has already stopped the DesktopThread using the surface.
    private synchronized void stopDesktopSurface() {
        clearDesktopSurfaceTexture();
        Surface surface = sDesktopSurface;
        sDesktopSurface = null;
        if (surface != null) {
            surface.release();
        }
        if (mDesktopTexture != null) {
            mDesktopTexture.release();
            mDesktopTexture = null;
        }
    }

    private native void setDesktopSurfaceTexture(SurfaceTexture surfaceTexture);
    private native void clearDesktopSurfaceTexture();
    private static native void setDesktopStreamInfo(int width, int height);
    // Desktop+ panel from the v6 frame header: flags (bit 0 = show), row-major 3x4 transform in
    // this headset's tracking space, width in metres.
    private static native void setDesktopPanel(int flags, float[] transform, float width);

    private native void setDecoderSurfaceTexture(SurfaceTexture surfaceTexture);
    private native void clearDecoderSurfaceTexture();
    private static native void setDecoderStreamInfo(int width, int height, int layout);
    // Which Flow pose (sequence sent over UDP) the PC rendered the frame with this pts from.
    private static native void setFramePoseSequence(long ptsUs, int poseSequence);
    // Head poses go back to whichever PC we are streaming from.
    private static native void setPoseTargetHost(String host);

    // Plays the PC's audio while the video stream is up; reconnects if the PC drops it
    // (e.g. the Windows default output changed sample rate).
    private static final class AudioThread extends Thread {
        private final String mHost;
        private volatile boolean mStop;
        private volatile Socket mSocket;

        AudioThread(String host) {
            super("FlowProbeAudio");
            mHost = host;
        }

        void requestStop() {
            mStop = true;
            Socket socket = mSocket;
            if (socket != null) {
                try {
                    socket.close();
                } catch (Exception ignored) {
                }
            }
            interrupt();
            try {
                join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }

        @Override
        public void run() {
            while (!mStop) {
                try {
                    playOnce();
                } catch (Exception e) {
                    if (!mStop) {
                        Log.w(TAG, "audio stream ended: " + e);
                    }
                }
                if (!mStop) {
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            Log.i(TAG, "audio thread stopped");
        }

        private void playOnce() throws Exception {
            Socket socket = new Socket();
            mSocket = socket;
            AudioTrack track = null;
            try {
                if (mStop) {
                    return;
                }
                socket.connect(new InetSocketAddress(mHost, AUDIO_PORT), SOCKET_CONNECT_TIMEOUT_MS);
                DataInputStream input = new DataInputStream(
                        new BufferedInputStream(socket.getInputStream(), 16 * 1024));
                byte[] magic = new byte[AUDIO_MAGIC.length()];
                input.readFully(magic);
                String magicText = new String(magic, StandardCharsets.US_ASCII);
                if (!AUDIO_MAGIC.equals(magicText)) {
                    throw new IllegalStateException("bad audio magic: " + magicText);
                }
                int version = input.readInt();
                int sampleRate = input.readInt();
                int channels = input.readInt();
                if (channels != 2 || sampleRate < 8000 || sampleRate > 192000) {
                    throw new IllegalStateException("unsupported audio format rate=" + sampleRate + " channels=" + channels);
                }

                final int bytesPerFrame = 4;
                int minBuffer = AudioTrack.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_OUT_STEREO,
                        AudioFormat.ENCODING_PCM_16BIT);
                int bufferBytes = Math.max(minBuffer, sampleRate * AUDIO_TRACK_BUFFER_MS / 1000 * bytesPerFrame);
                AudioTrack.Builder builder = new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                .build())
                        .setAudioFormat(new AudioFormat.Builder()
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setSampleRate(sampleRate)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                                .build())
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .setBufferSizeInBytes(bufferBytes);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
                }
                track = builder.build();
                track.play();
                Log.i(TAG, "audio started host=" + mHost + ":" + AUDIO_PORT + " version=" + version
                        + " rate=" + sampleRate + " trackBuffer=" + track.getBufferSizeInFrames()
                        + " minBuffer=" + minBuffer / bytesPerFrame);

                byte[] chunk = new byte[sampleRate * AUDIO_CHUNK_MS / 1000 * bytesPerFrame];
                byte[] pad = new byte[sampleRate * AUDIO_REFILL_PAD_MS / 1000 * bytesPerFrame];
                long maxQueuedFrames = (long) sampleRate * AUDIO_MAX_QUEUED_MS / 1000;
                long writtenFrames = 0;
                int received = 0;
                int dropped = 0;
                int refills = 0;
                long nextLogNs = System.nanoTime() + 10000000000L;
                while (!mStop) {
                    input.readFully(chunk);
                    ++received;
                    long queuedFrames = writtenFrames - (track.getPlaybackHeadPosition() & 0xffffffffL);
                    if (queuedFrames > maxQueuedFrames) {
                        ++dropped;
                    } else {
                        if (queuedFrames <= 0) {
                            track.write(pad, 0, pad.length);
                            writtenFrames += pad.length / bytesPerFrame;
                            ++refills;
                        }
                        track.write(chunk, 0, chunk.length);
                        writtenFrames += chunk.length / bytesPerFrame;
                    }
                    long now = System.nanoTime();
                    if (now >= nextLogNs) {
                        nextLogNs = now + 10000000000L;
                        Log.i(TAG, "audio chunks=" + received + " dropped=" + dropped + " refills=" + refills
                                + " queuedMs=" + queuedFrames * 1000 / sampleRate
                                + " underruns=" + track.getUnderrunCount());
                        received = 0;
                        dropped = 0;
                        refills = 0;
                    }
                }
            } finally {
                mSocket = null;
                try {
                    socket.close();
                } catch (Exception ignored) {
                }
                if (track != null) {
                    try {
                        track.stop();
                    } catch (Exception ignored) {
                    }
                    track.release();
                }
            }
        }
    }

    // Receives the desktop stream (TCP 8005) and decodes it into sDesktopSurface; reconnects
    // while the main video stream is up. Nothing listening = no desktop layer.
    private static final class DesktopThread extends Thread {
        private final String mHost;
        private volatile boolean mStop;
        private volatile Socket mSocket;

        DesktopThread(String host) {
            super("FlowProbeDesktop");
            mHost = host;
        }

        void requestStop() {
            mStop = true;
            Socket socket = mSocket;
            if (socket != null) {
                try {
                    socket.close();
                } catch (Exception ignored) {
                }
            }
            interrupt();
            try {
                join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }

        @Override
        public void run() {
            while (!mStop) {
                try {
                    playOnce();
                } catch (Exception e) {
                    if (!mStop) {
                        Log.w(TAG, "desktop stream ended: " + e);
                    }
                }
                setDesktopStreamInfo(0, 0);
                if (!mStop) {
                    try {
                        Thread.sleep(300); // e.g. the PC switched monitors and closed the stream
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            Log.i(TAG, "desktop thread stopped");
        }

        private static byte[] readBlob(DataInputStream input) throws Exception {
            int length = input.readInt();
            if (length < 0 || length > 1024 * 1024) {
                throw new IllegalStateException("bad desktop header blob: " + length);
            }
            byte[] blob = new byte[length];
            input.readFully(blob);
            return blob;
        }

        private void playOnce() throws Exception {
            Surface surface = sDesktopSurface;
            if (surface == null || mStop) {
                return;
            }
            Socket socket = new Socket();
            mSocket = socket;
            MediaCodec codec = null;
            try {
                socket.connect(new InetSocketAddress(mHost, DESKTOP_PORT), SOCKET_CONNECT_TIMEOUT_MS);
                // No read timeout: the PC sends nothing while the dashboard is closed (e.g. in a
                // game), and the stream must still be up when it reopens. This thread ends with
                // the main video stream anyway (requestStop closes the socket).
                socket.setSoTimeout(0);
                DataInputStream input = new DataInputStream(new BufferedInputStream(socket.getInputStream(), 256 * 1024));
                byte[] magic = new byte[SOCKET_MAGIC.length()];
                input.readFully(magic);
                if (!SOCKET_MAGIC.equals(new String(magic, StandardCharsets.US_ASCII))) {
                    throw new IllegalStateException("bad desktop stream magic");
                }
                int version = input.readInt();
                int width = input.readInt();
                int height = input.readInt();
                int fps = input.readInt();
                if (version >= 4) {
                    input.readInt(); // layout: always mono here
                }
                byte[] sps = readBlob(input);
                byte[] pps = readBlob(input);

                MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
                format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
                format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
                format.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
                codec = MediaCodec.createDecoderByType("video/avc");
                codec.configure(format, surface, null, 0);
                codec.start();
                setDesktopStreamInfo(width, height);
                Log.i(TAG, "desktop stream started host=" + mHost + ":" + DESKTOP_PORT + " version=" + version
                        + " size=" + width + "x" + height + " fps=" + fps + " codec=" + codec.getName());

                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                long windowStartNs = System.nanoTime();
                int received = 0;
                int rendered = 0;
                while (!mStop) {
                    int size = input.readInt();
                    if (size < 0) {
                        break;
                    }
                    if (size > 4 * 1024 * 1024) {
                        throw new IllegalStateException("oversized desktop frame: " + size);
                    }
                    long ptsUs = input.readLong();
                    if (version >= 3) {
                        input.readLong();
                        input.readLong();
                        if (version >= 5) {
                            input.readInt();
                        }
                    } else if (version >= 2) {
                        input.readLong();
                    }
                    byte[] frame = new byte[size];
                    input.readFully(frame);
                    if (version >= 3) {
                        input.readLong();
                    }
                    ++received;

                    int inputIndex = -1;
                    while (!mStop && inputIndex < 0) {
                        inputIndex = codec.dequeueInputBuffer(5000);
                        rendered += drain(codec, info);
                    }
                    if (inputIndex >= 0) {
                        ByteBuffer buffer = codec.getInputBuffer(inputIndex);
                        buffer.clear();
                        buffer.put(frame);
                        codec.queueInputBuffer(inputIndex, 0, frame.length, ptsUs, 0);
                    }
                    rendered += drain(codec, info);

                    long now = System.nanoTime();
                    if (now - windowStartNs >= 10000000000L) {
                        double seconds = (now - windowStartNs) / 1e9;
                        Log.i(TAG, String.format(java.util.Locale.US, "desktop rates recvFps=%.1f renderedFps=%.1f",
                                received / seconds, rendered / seconds));
                        windowStartNs = now;
                        received = 0;
                        rendered = 0;
                    }
                }
            } finally {
                mSocket = null;
                try {
                    socket.close();
                } catch (Exception ignored) {
                }
                if (codec != null) {
                    try {
                        codec.stop();
                    } catch (Exception ignored) {
                    }
                    codec.release();
                }
            }
        }

        // Renders every decoded frame to the surface (the renderer latches the newest one).
        private static int drain(MediaCodec codec, MediaCodec.BufferInfo info) {
            int rendered = 0;
            while (true) {
                int outputIndex = codec.dequeueOutputBuffer(info, 0);
                if (outputIndex < 0) {
                    return rendered;
                }
                codec.releaseOutputBuffer(outputIndex, info.size > 0);
                if (info.size > 0) {
                    ++rendered;
                }
            }
        }
    }

    private static final class DecoderThread extends Thread {
        private final AssetManager mAssets;
        private final Surface mSurface;
        private volatile boolean mStop;
        private DecodeStats mSocketStats;
        // Per-2s throughput: frames read off the socket vs frames the decoder rendered.
        private long mRateWindowStartNs;
        private int mRateRecv;
        private int mRateOut;

        DecoderThread(AssetManager assets, Surface surface) {
            super("FlowProbeDecoder");
            mAssets = assets;
            mSurface = surface;
        }

        void requestStop() {
            mStop = true;
            interrupt();
        }

        @Override
        public void run() {
            while (!mStop) {
                playOnce();
            }
            Log.i(TAG, "decoder thread stopped");
        }

        private void playOnce() {
            if (USE_SOCKET_H264_FEEDER && playSocketH264Once()) {
                return;
            }

            if (USE_RAW_H264_FEEDER) {
                playRawH264Once();
                return;
            }

            MediaExtractor extractor = new MediaExtractor();
            MediaCodec codec = null;
            try {
                String source = openExtractor(extractor);
                int videoTrack = selectVideoTrack(extractor);
                if (videoTrack < 0) {
                    Log.e(TAG, "no video track in " + source);
                    return;
                }

                extractor.selectTrack(videoTrack);
                MediaFormat format = extractor.getTrackFormat(videoTrack);
                String mime = format.getString(MediaFormat.KEY_MIME);
                codec = MediaCodec.createDecoderByType(mime);
                codec.configure(format, mSurface, null, 0);
                codec.start();
                Log.i(TAG, "decoder started source=" + source + " mime=" + mime + " format=" + format);

                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                boolean inputDone = false;
                boolean outputDone = false;
                long firstPtsUs = -1;
                long firstWallMs = -1;

                while (!mStop && !outputDone) {
                    if (!inputDone) {
                        int inputIndex = codec.dequeueInputBuffer(10000);
                        if (inputIndex >= 0) {
                            ByteBuffer input = codec.getInputBuffer(inputIndex);
                            int sampleSize = extractor.readSampleData(input, 0);
                            if (sampleSize < 0) {
                                codec.queueInputBuffer(inputIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inputDone = true;
                            } else {
                                long ptsUs = extractor.getSampleTime();
                                codec.queueInputBuffer(inputIndex, 0, sampleSize, ptsUs, 0);
                                extractor.advance();
                            }
                        }
                    }

                    int outputIndex = codec.dequeueOutputBuffer(info, 10000);
                    if (outputIndex >= 0) {
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            outputDone = true;
                        }
                        if (info.size > 0) {
                            if (firstPtsUs < 0) {
                                firstPtsUs = info.presentationTimeUs;
                                firstWallMs = System.nanoTime() / 1000000L;
                            }
                            paceFrame(firstPtsUs, firstWallMs, info.presentationTimeUs);
                            codec.releaseOutputBuffer(outputIndex, true);
                        } else {
                            codec.releaseOutputBuffer(outputIndex, false);
                        }
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "decoder failed", e);
                mStop = true;
            } finally {
                if (codec != null) {
                    try {
                        codec.stop();
                    } catch (Exception ignored) {
                    }
                    codec.release();
                }
                extractor.release();
            }
        }

        private boolean playSocketH264Once() {
            MediaCodec codec = null;
            Socket socket = null;
            AudioThread audio = null;
            DesktopThread desktop = null;
            try {
                socket = connectSocket();
                socket.setSoTimeout(SOCKET_READ_TIMEOUT_MS);
                setPoseTargetHost(socket.getInetAddress().getHostAddress());
                audio = new AudioThread(socket.getInetAddress().getHostAddress());
                audio.start();
                desktop = new DesktopThread(socket.getInetAddress().getHostAddress());
                desktop.start();

                DataInputStream input = new DataInputStream(
                        new BufferedInputStream(socket.getInputStream(), 64 * 1024));
                byte[] magic = new byte[SOCKET_MAGIC.length()];
                input.readFully(magic);
                String magicText = new String(magic, StandardCharsets.US_ASCII);
                if (!SOCKET_MAGIC.equals(magicText)) {
                    throw new IllegalStateException("bad socket magic: " + magicText);
                }

                int version = input.readInt();
                int width = input.readInt();
                int height = input.readInt();
                int fps = input.readInt();
                // v4 adds layout: 0 = mono desktop on a world-locked screen,
                // 1 = side-by-side stereo eyes rendered by SteamVR.
                int layout = version >= 4 ? input.readInt() : STREAM_LAYOUT_MONO;
                byte[] sps = readLengthPrefixedBytes(input, 1024 * 1024);
                byte[] pps = readLengthPrefixedBytes(input, 1024 * 1024);
                setDecoderStreamInfo(width, height, layout);

                MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
                format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
                format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
                format.setByteBuffer("csd-1", ByteBuffer.wrap(pps));

                String decoderMode = getSystemProperty(DECODER_MODE_PROP, DECODER_MODE_BASELINE);
                codec = MediaCodec.createDecoderByType("video/avc");
                String codecName = codec.getName();
                MediaCodecInfo codecInfo = findCodecInfo(codecName);
                DecoderCapabilityProbe capabilityProbe = probeDecoderCapabilities(codec, codecInfo, "video/avc");
                applyDecoderFormatConfig(format, fps, decoderMode, capabilityProbe);
                Log.i(TAG, "decoder config mode=" + decoderMode + " format=" + format);
                codec.configure(format, mSurface, null, 0);
                codec.start();
                applyDecoderRuntimeConfig(codec, decoderMode, capabilityProbe);
                Log.i(TAG, "socket decoder started host=" + socket.getInetAddress().getHostAddress() + ":" + SOCKET_PORT
                        + " version=" + version
                        + " layout=" + layout
                        + " size=" + width + "x" + height
                        + " fps=" + fps
                        + " codec=" + codecName
                        + " software=" + isLikelySoftwareCodec(codecName)
                        + " mode=" + decoderMode
                        + " sps=" + sps.length
                        + " pps=" + pps.length);

                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                float[] panelTransform = new float[12];
                // Live stream: release frames as soon as they decode (no pacing, see paceFrame).
                long firstWallMs = -1;
                int queuedFrames = 0;
                long totalNetworkLagMs = 0;
                long maxNetworkLagMs = 0;
                mSocketStats = new DecodeStats();

                while (!mStop) {
                    long frameReadStartMs = System.currentTimeMillis();
                    long frameReadStartNs = System.nanoTime();
                    int size;
                    try {
                        size = input.readInt();
                    } catch (EOFException e) {
                        break;
                    }
                    if (size < 0) {
                        break;
                    }
                    if (size > 2 * 1024 * 1024) {
                        throw new IllegalStateException("oversized frame: " + size);
                    }

                    long ptsUs = input.readLong();
                    long encodedReadyMs = -1;
                    long sendStartMs = -1;
                    long sendEndMs = -1;
                    long sentEpochMs = -1;
                    int poseSequence = 0;
                    if (version >= 3) {
                        encodedReadyMs = input.readLong();
                        sendStartMs = input.readLong();
                        sentEpochMs = sendStartMs;
                        if (version >= 5) {
                            poseSequence = input.readInt();
                        }
                        if (version >= 6) {
                            int panelFlags = input.readInt();
                            for (int i = 0; i < 12; ++i) {
                                panelTransform[i] = input.readFloat();
                            }
                            setDesktopPanel(panelFlags, panelTransform, input.readFloat());
                        }
                    } else if (version >= 2) {
                        sentEpochMs = input.readLong();
                        encodedReadyMs = sentEpochMs;
                        sendStartMs = sentEpochMs;
                    }
                    byte[] frame = new byte[size];
                    input.readFully(frame);
                    if (version >= 3) {
                        sendEndMs = input.readLong();
                    } else {
                        sendEndMs = sentEpochMs;
                    }
                    long receiveDoneMs = System.currentTimeMillis();
                    long receiveDoneNs = System.nanoTime();
                    mSocketStats.recordReceive(encodedReadyMs, sendStartMs, sendEndMs,
                            frameReadStartMs, receiveDoneMs, frameReadStartNs, receiveDoneNs, ptsUs);
                    if (sentEpochMs > 0) {
                        long lagMs = System.currentTimeMillis() - sentEpochMs;
                        if (lagMs >= 0 && lagMs < 60000) {
                            totalNetworkLagMs += lagMs;
                            if (lagMs > maxNetworkLagMs) {
                                maxNetworkLagMs = lagMs;
                            }
                        }
                    }
                    if (poseSequence != 0) {
                        setFramePoseSequence(ptsUs, poseSequence);
                    }
                    ++mRateRecv;
                    logRatesIfDue();
                    queueFrame(codec, info, frame, ptsUs, firstWallMs);
                    ++queuedFrames;
                }

                queueEndOfStream(codec, inputPtsUsForFrame(queuedFrames, fps));
                drainOutput(codec, info, 0, firstWallMs, true);
                long avgNetworkLagMs = queuedFrames > 0 ? totalNetworkLagMs / queuedFrames : 0;
                Log.i(TAG, "socket decoder completed queuedFrames=" + queuedFrames
                        + " avgNetworkLagMs=" + avgNetworkLagMs
                        + " maxNetworkLagMs=" + maxNetworkLagMs
                        + " " + mSocketStats.summary());
                return true;
            } catch (Exception e) {
                Log.w(TAG, "socket decoder unavailable, falling back to raw asset", e);
                return false;
            } finally {
                if (audio != null) {
                    audio.requestStop();
                }
                if (desktop != null) {
                    desktop.requestStop();
                }
                if (codec != null) {
                    try {
                        codec.stop();
                    } catch (Exception ignored) {
                    }
                    codec.release();
                }
                if (socket != null) {
                    try {
                        socket.close();
                    } catch (Exception ignored) {
                    }
                }
                mSocketStats = null;
            }
        }

        private Socket connectSocket() throws Exception {
            Exception lastError = null;
            String discoveredHost = discoverSocketHost();
            if (discoveredHost != null) {
                Socket socket = new Socket();
                try {
                    socket.connect(new InetSocketAddress(discoveredHost, SOCKET_PORT), SOCKET_CONNECT_TIMEOUT_MS);
                    Log.i(TAG, "socket connected discoveredHost=" + discoveredHost + ":" + SOCKET_PORT);
                    return socket;
                } catch (Exception e) {
                    lastError = e;
                    try {
                        socket.close();
                    } catch (Exception ignored) {
                    }
                    Log.w(TAG, "socket connect failed discoveredHost=" + discoveredHost + ":" + SOCKET_PORT, e);
                }
            }

            for (int i = 0; i < SOCKET_HOSTS.length; ++i) {
                String host = SOCKET_HOSTS[i];
                Socket socket = new Socket();
                try {
                    socket.connect(new InetSocketAddress(host, SOCKET_PORT), SOCKET_CONNECT_TIMEOUT_MS);
                    Log.i(TAG, "socket connected host=" + host + ":" + SOCKET_PORT);
                    return socket;
                } catch (Exception e) {
                    lastError = e;
                    try {
                        socket.close();
                    } catch (Exception ignored) {
                    }
                    Log.w(TAG, "socket connect failed host=" + host + ":" + SOCKET_PORT, e);
                }
            }
            throw lastError != null ? lastError : new IllegalStateException("no socket hosts configured");
        }

        private String discoverSocketHost() {
            DatagramSocket socket = null;
            try {
                socket = new DatagramSocket(SOCKET_DISCOVERY_PORT);
                socket.setSoTimeout(SOCKET_DISCOVERY_TIMEOUT_MS);
                long deadline = System.currentTimeMillis() + SOCKET_DISCOVERY_TIMEOUT_MS;
                byte[] buffer = new byte[128];
                while (System.currentTimeMillis() < deadline && !mStop) {
                    DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                    try {
                        socket.receive(packet);
                    } catch (SocketTimeoutException e) {
                        break;
                    }
                    String message = new String(packet.getData(), packet.getOffset(), packet.getLength(),
                            StandardCharsets.US_ASCII).trim();
                    if (message.startsWith(SOCKET_DISCOVERY_MAGIC)) {
                        String host = packet.getAddress().getHostAddress();
                        Log.i(TAG, "discovered socket sender host=" + host + " message=" + message);
                        return host;
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "socket discovery failed", e);
            } finally {
                if (socket != null) {
                    socket.close();
                }
            }
            return null;
        }

        private String getSystemProperty(String name, String defaultValue) {
            try {
                Class<?> propertiesClass = Class.forName("android.os.SystemProperties");
                Method getMethod = propertiesClass.getMethod("get", String.class, String.class);
                Object value = getMethod.invoke(null, name, defaultValue);
                return value != null ? value.toString() : defaultValue;
            } catch (Exception e) {
                Log.w(TAG, "system property unavailable name=" + name, e);
                return defaultValue;
            }
        }

        private MediaCodecInfo findCodecInfo(String codecName) {
            try {
                MediaCodecList codecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
                MediaCodecInfo[] infos = codecList.getCodecInfos();
                for (int i = 0; i < infos.length; ++i) {
                    if (infos[i].getName().equals(codecName)) {
                        return infos[i];
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "codec info lookup failed codec=" + codecName, e);
            }
            return null;
        }

        private DecoderCapabilityProbe probeDecoderCapabilities(MediaCodec codec,
                                                               MediaCodecInfo codecInfo,
                                                               String mime) {
            DecoderCapabilityProbe probe = new DecoderCapabilityProbe();
            String codecName = codec.getName();
            probe.codecName = codecName;
            probe.software = isLikelySoftwareCodec(codecName);
            if (codecInfo != null) {
                probe.encoder = codecInfo.isEncoder();
                try {
                    MediaCodecInfo.CodecCapabilities capabilities =
                            codecInfo.getCapabilitiesForType(mime);
                    probe.featureLowLatency = capabilities.isFeatureSupported(KEY_LOW_LATENCY);
                } catch (Exception e) {
                    Log.w(TAG, "codec capability lookup failed codec=" + codecName, e);
                }
            }
            probe.vendorParameters.addAll(getSupportedVendorParameters(codec));
            for (int i = 0; i < probe.vendorParameters.size(); ++i) {
                String name = probe.vendorParameters.get(i);
                String lower = name.toLowerCase();
                if (lower.contains("latency") || lower.contains("low-latency")) {
                    probe.latencyVendorParameters.add(name);
                }
                if (KEY_VENDOR_QTI_LOW_LATENCY.equals(name)) {
                    probe.hasQtiLowLatency = true;
                }
            }
            Log.i(TAG, "decoder capability codec=" + codecName
                    + " software=" + probe.software
                    + " encoder=" + probe.encoder
                    + " featureLowLatency=" + probe.featureLowLatency
                    + " vendorParamCount=" + probe.vendorParameters.size()
                    + " latencyVendorParams=" + probe.latencyVendorParameters
                    + " hasQtiLowLatency=" + probe.hasQtiLowLatency);
            return probe;
        }

        @SuppressWarnings("unchecked")
        private ArrayList<String> getSupportedVendorParameters(MediaCodec codec) {
            ArrayList<String> parameters = new ArrayList<String>();
            try {
                Method method = codec.getClass().getMethod("getSupportedVendorParameters");
                Object result = method.invoke(codec);
                if (result instanceof List) {
                    List<?> list = (List<?>) result;
                    for (int i = 0; i < list.size(); ++i) {
                        Object value = list.get(i);
                        if (value != null) {
                            parameters.add(value.toString());
                        }
                    }
                }
            } catch (NoSuchMethodException e) {
                Log.i(TAG, "decoder vendor parameters unsupported on this Android API");
            } catch (Exception e) {
                Log.w(TAG, "decoder vendor parameter query failed", e);
            }
            return parameters;
        }

        private void applyDecoderFormatConfig(MediaFormat format, int fps, String decoderMode,
                                              DecoderCapabilityProbe probe) {
            if (!DECODER_MODE_LOW_LATENCY.equals(decoderMode)) {
                return;
            }
            format.setInteger(MediaFormat.KEY_PRIORITY, 0);
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, Math.max(240, fps * 4));
            format.setInteger(KEY_LOW_LATENCY, 1);
            if (probe.hasQtiLowLatency) {
                format.setInteger(KEY_VENDOR_QTI_LOW_LATENCY, 1);
            }
        }

        private void applyDecoderRuntimeConfig(MediaCodec codec, String decoderMode,
                                               DecoderCapabilityProbe probe) {
            if (!DECODER_MODE_LOW_LATENCY.equals(decoderMode)) {
                return;
            }
            Bundle params = new Bundle();
            params.putInt(KEY_LOW_LATENCY, 1);
            if (probe.hasQtiLowLatency) {
                params.putInt(KEY_VENDOR_QTI_LOW_LATENCY, 1);
            }
            try {
                codec.setParameters(params);
                Log.i(TAG, "decoder runtime params applied keys=" + params.keySet());
            } catch (Exception e) {
                Log.w(TAG, "decoder runtime params rejected keys=" + params.keySet(), e);
            }
        }

        private boolean isLikelySoftwareCodec(String codecName) {
            String name = codecName != null ? codecName.toLowerCase() : "";
            return name.startsWith("omx.google.")
                    || name.startsWith("c2.android.")
                    || name.contains("sw")
                    || name.contains("software");
        }

        private void playRawH264Once() {
            MediaCodec codec = null;
            try {
                byte[] stream = readAsset(TEST_RAW_H264_ASSET);
                ArrayList<int[]> nals = findNalRanges(stream);
                ByteBuffer sps = findCsdBuffer(stream, nals, 7);
                ByteBuffer pps = findCsdBuffer(stream, nals, 8);

                MediaFormat format = MediaFormat.createVideoFormat("video/avc", 1280, 720);
                format.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
                if (sps != null) {
                    format.setByteBuffer("csd-0", sps);
                }
                if (pps != null) {
                    format.setByteBuffer("csd-1", pps);
                }

                codec = MediaCodec.createDecoderByType("video/avc");
                codec.configure(format, mSurface, null, 0);
                codec.start();
                Log.i(TAG, "raw decoder started asset=" + TEST_RAW_H264_ASSET
                        + " nals=" + nals.size()
                        + " hasSps=" + (sps != null)
                        + " hasPps=" + (pps != null));

                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                long firstWallMs = System.nanoTime() / 1000000L;
                long ptsUs = 0;
                int queuedFrames = 0;

                for (int i = 0; i < nals.size() && !mStop; ++i) {
                    int[] range = nals.get(i);
                    int nalType = nalType(stream, range[0], range[1]);
                    if (!isVclNal(nalType)) {
                        continue;
                    }

                    int inputIndex = codec.dequeueInputBuffer(10000);
                    if (inputIndex < 0) {
                        drainOutput(codec, info, ptsUs, firstWallMs, false);
                        --i;
                        continue;
                    }

                    ByteBuffer input = codec.getInputBuffer(inputIndex);
                    input.clear();
                    input.put(stream, range[0], range[1] - range[0]);
                    codec.queueInputBuffer(inputIndex, 0, range[1] - range[0], ptsUs, 0);
                    drainOutput(codec, info, ptsUs, firstWallMs, false);

                    ptsUs += 33333L;
                    ++queuedFrames;
                }

                int inputIndex = codec.dequeueInputBuffer(10000);
                if (inputIndex >= 0) {
                    codec.queueInputBuffer(inputIndex, 0, 0, ptsUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                }
                drainOutput(codec, info, ptsUs, firstWallMs, true);
                Log.i(TAG, "raw decoder completed queuedFrames=" + queuedFrames);
            } catch (Exception e) {
                Log.e(TAG, "raw decoder failed", e);
                mStop = true;
            } finally {
                if (codec != null) {
                    try {
                        codec.stop();
                    } catch (Exception ignored) {
                    }
                    codec.release();
                }
            }
        }

        private byte[] readLengthPrefixedBytes(DataInputStream input, int maxSize) throws Exception {
            int size = input.readInt();
            if (size <= 0 || size > maxSize) {
                throw new IllegalStateException("invalid blob size: " + size);
            }
            byte[] data = new byte[size];
            input.readFully(data);
            return data;
        }

        private void queueFrame(MediaCodec codec, MediaCodec.BufferInfo info,
                                byte[] frame, long ptsUs, long firstWallMs) throws Exception {
            while (!mStop) {
                long queueStartNs = System.nanoTime();
                int inputIndex = codec.dequeueInputBuffer(10000);
                long inputReadyNs = System.nanoTime();
                if (inputIndex >= 0) {
                    ByteBuffer input = codec.getInputBuffer(inputIndex);
                    input.clear();
                    if (frame.length > input.capacity()) {
                        throw new IllegalStateException("frame " + frame.length
                                + " exceeds input capacity " + input.capacity());
                    }
                    input.put(frame);
                    codec.queueInputBuffer(inputIndex, 0, frame.length, ptsUs, 0);
                    long queueDoneNs = System.nanoTime();
                    if (mSocketStats != null) {
                        mSocketStats.recordQueue(ptsUs, queueStartNs, inputReadyNs, queueDoneNs);
                    }
                    drainOutput(codec, info, ptsUs, firstWallMs, false);
                    return;
                }
                drainOutput(codec, info, ptsUs, firstWallMs, false);
            }
        }

        private void queueEndOfStream(MediaCodec codec, long ptsUs) throws Exception {
            while (!mStop) {
                int inputIndex = codec.dequeueInputBuffer(10000);
                if (inputIndex >= 0) {
                    codec.queueInputBuffer(inputIndex, 0, 0, ptsUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    return;
                }
            }
        }

        private long inputPtsUsForFrame(int frameIndex, int fps) {
            int safeFps = fps > 0 ? fps : 30;
            return frameIndex * 1000000L / safeFps;
        }

        private byte[] readAsset(String name) throws Exception {
            InputStream input = mAssets.open(name);
            try {
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                byte[] buffer = new byte[16384];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    output.write(buffer, 0, read);
                }
                return output.toByteArray();
            } finally {
                input.close();
            }
        }

        private ArrayList<int[]> findNalRanges(byte[] data) {
            ArrayList<int[]> ranges = new ArrayList<int[]>();
            int start = findStartCode(data, 0);
            while (start >= 0) {
                int next = findStartCode(data, start + startCodeLength(data, start));
                int end = next >= 0 ? next : data.length;
                ranges.add(new int[]{start, end});
                start = next;
            }
            return ranges;
        }

        private int findStartCode(byte[] data, int from) {
            for (int i = from; i + 3 < data.length; ++i) {
                if (data[i] == 0 && data[i + 1] == 0) {
                    if (data[i + 2] == 1) {
                        return i;
                    }
                    if (i + 4 < data.length && data[i + 2] == 0 && data[i + 3] == 1) {
                        return i;
                    }
                }
            }
            return -1;
        }

        private int startCodeLength(byte[] data, int start) {
            return data[start + 2] == 1 ? 3 : 4;
        }

        private int nalType(byte[] data, int start, int end) {
            int header = start + startCodeLength(data, start);
            if (header >= end) {
                return -1;
            }
            return data[header] & 0x1f;
        }

        private boolean isVclNal(int nalType) {
            return nalType >= 1 && nalType <= 5;
        }

        private ByteBuffer findCsdBuffer(byte[] data, ArrayList<int[]> nals, int targetNalType) {
            for (int i = 0; i < nals.size(); ++i) {
                int[] range = nals.get(i);
                if (nalType(data, range[0], range[1]) == targetNalType) {
                    byte[] csd = new byte[range[1] - range[0]];
                    System.arraycopy(data, range[0], csd, 0, csd.length);
                    return ByteBuffer.wrap(csd);
                }
            }
            return null;
        }

        private void drainOutput(MediaCodec codec, MediaCodec.BufferInfo info,
                                 long latestPtsUs, long firstWallMs, boolean waitForEos) {
            while (!mStop) {
                int outputIndex = codec.dequeueOutputBuffer(info, waitForEos ? 10000 : 0);
                if (outputIndex >= 0) {
                    if (info.size > 0) {
                        if (mSocketStats != null) {
                            mSocketStats.recordOutput(info.presentationTimeUs, System.nanoTime());
                        }
                        paceFrame(0, firstWallMs, info.presentationTimeUs);
                        long releaseStartNs = System.nanoTime();
                        codec.releaseOutputBuffer(outputIndex, true);
                        long releaseDoneNs = System.nanoTime();
                        ++mRateOut;
                        if (mSocketStats != null) {
                            mSocketStats.recordRelease(info.presentationTimeUs, releaseStartNs, releaseDoneNs);
                        }
                    } else {
                        codec.releaseOutputBuffer(outputIndex, false);
                    }
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        return;
                    }
                } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    Log.i(TAG, "decoder output format=" + codec.getOutputFormat());
                } else {
                    return;
                }
            }
        }

        private String openExtractor(MediaExtractor extractor) throws Exception {
            try {
                probeUrl(TEST_VIDEO_URL);
                extractor.setDataSource(TEST_VIDEO_URL);
                Log.i(TAG, "decoder source url=" + TEST_VIDEO_URL);
                return TEST_VIDEO_URL;
            } catch (Exception e) {
                Log.w(TAG, "decoder url source unavailable, falling back to asset", e);
            }

            AssetFileDescriptor afd = mAssets.openFd(TEST_VIDEO_ASSET);
            extractor.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            afd.close();
            Log.i(TAG, "decoder source asset=" + TEST_VIDEO_ASSET);
            return TEST_VIDEO_ASSET;
        }

        private void probeUrl(String urlText) throws Exception {
            HttpURLConnection connection = null;
            try {
                URL url = new URL(urlText);
                connection = (HttpURLConnection) url.openConnection();
                connection.setConnectTimeout(2000);
                connection.setReadTimeout(2000);
                connection.setRequestMethod("GET");
                connection.connect();
                Log.i(TAG, "decoder url probe code=" + connection.getResponseCode()
                        + " length=" + connection.getContentLength()
                        + " type=" + connection.getContentType());
            } finally {
                if (connection != null) {
                    connection.disconnect();
                }
            }
        }

        private int selectVideoTrack(MediaExtractor extractor) {
            for (int i = 0; i < extractor.getTrackCount(); ++i) {
                MediaFormat format = extractor.getTrackFormat(i);
                String mime = format.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("video/")) {
                    return i;
                }
            }
            return -1;
        }

        private void logRatesIfDue() {
            long nowNs = System.nanoTime();
            if (mRateWindowStartNs == 0) {
                mRateWindowStartNs = nowNs;
                mRateRecv = 0;
                mRateOut = 0;
                return;
            }
            double seconds = (nowNs - mRateWindowStartNs) / 1e9;
            if (seconds < 2.0) {
                return;
            }
            Log.i(TAG, String.format(java.util.Locale.US, "stream rates recvFps=%.1f decodedFps=%.1f",
                    mRateRecv / seconds, mRateOut / seconds));
            mRateWindowStartNs = nowNs;
            mRateRecv = 0;
            mRateOut = 0;
        }

        private void paceFrame(long firstPtsUs, long firstWallMs, long ptsUs) {
            if (firstWallMs < 0) {
                return;
            }
            long targetMs = firstWallMs + (ptsUs - firstPtsUs) / 1000L;
            long nowMs = System.nanoTime() / 1000000L;
            long sleepMs = targetMs - nowMs;
            if (sleepMs > 1 && sleepMs < 100) {
                try {
                    Thread.sleep(sleepMs);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }

        private static final class DecoderCapabilityProbe {
            String codecName = "";
            boolean software;
            boolean encoder;
            boolean featureLowLatency;
            boolean hasQtiLowLatency;
            final ArrayList<String> vendorParameters = new ArrayList<String>();
            final ArrayList<String> latencyVendorParameters = new ArrayList<String>();
        }

        private static final class DecodeStats {
            private int receivedFrames;
            private int queuedFrames;
            private int outputFrames;
            private int releasedFrames;
            private long totalEncodedToReceiveMs;
            private long maxEncodedToReceiveMs;
            private long totalPcSendMs;
            private long maxPcSendMs;
            private long totalFlowReadMs;
            private long maxFlowReadMs;
            private long totalQueueWaitMs;
            private long maxQueueWaitMs;
            private long totalQueueCallMs;
            private long maxQueueCallMs;
            private long totalQueueToOutputMs;
            private long maxQueueToOutputMs;
            private long totalReleaseCallUs;
            private long maxReleaseCallUs;
            private long totalOutputToReleaseMs;
            private long maxOutputToReleaseMs;
            private long totalDepthAfterQueue;
            private long maxDepthAfterQueue;
            private long totalDepthAtOutput;
            private long maxDepthAtOutput;
            private final int[] depthAfterQueueBuckets = new int[16];
            private final int[] depthAtOutputBuckets = new int[16];
            private final HashMap<Long, FrameStats> framesByPts = new HashMap<Long, FrameStats>();
            private final ArrayList<Long> queueToOutputMsValues = new ArrayList<Long>();

            void recordReceive(long encodedReadyMs, long sendStartMs, long sendEndMs,
                               long flowReadStartMs, long flowReceiveDoneMs,
                               long socketFirstByteNs, long socketLastByteNs, long ptsUs) {
                ++receivedFrames;
                long flowReadMs = flowReceiveDoneMs - flowReadStartMs;
                totalFlowReadMs += flowReadMs;
                maxFlowReadMs = Math.max(maxFlowReadMs, flowReadMs);
                FrameStats frame = frameForPts(ptsUs);
                frame.socketFirstByteNs = socketFirstByteNs;
                frame.socketLastByteNs = socketLastByteNs;

                if (encodedReadyMs > 0) {
                    long encodedToReceiveMs = flowReceiveDoneMs - encodedReadyMs;
                    if (encodedToReceiveMs >= 0 && encodedToReceiveMs < 60000) {
                        totalEncodedToReceiveMs += encodedToReceiveMs;
                        maxEncodedToReceiveMs = Math.max(maxEncodedToReceiveMs, encodedToReceiveMs);
                    }
                }
                if (sendStartMs > 0 && sendEndMs >= sendStartMs) {
                    long pcSendMs = sendEndMs - sendStartMs;
                    totalPcSendMs += pcSendMs;
                    maxPcSendMs = Math.max(maxPcSendMs, pcSendMs);
                }
            }

            void recordQueue(long ptsUs, long queueStartNs, long inputReadyNs, long queueDoneNs) {
                ++queuedFrames;
                long queueWaitMs = (inputReadyNs - queueStartNs) / 1000000L;
                long queueCallMs = (queueDoneNs - inputReadyNs) / 1000000L;
                totalQueueWaitMs += queueWaitMs;
                totalQueueCallMs += queueCallMs;
                maxQueueWaitMs = Math.max(maxQueueWaitMs, queueWaitMs);
                maxQueueCallMs = Math.max(maxQueueCallMs, queueCallMs);
                FrameStats frame = frameForPts(ptsUs);
                frame.queueStartNs = queueStartNs;
                frame.inputReadyNs = inputReadyNs;
                frame.queueDoneNs = queueDoneNs;
                int depth = queuedFrames - outputFrames;
                totalDepthAfterQueue += depth;
                maxDepthAfterQueue = Math.max(maxDepthAfterQueue, depth);
                addDepth(depthAfterQueueBuckets, depth);
            }

            void recordOutput(long ptsUs, long outputReadyNs) {
                FrameStats frame = framesByPts.get(ptsUs);
                if (frame == null || frame.queueDoneNs == 0) {
                    return;
                }
                ++outputFrames;
                frame.outputReadyNs = outputReadyNs;
                long queueToOutputMs = (outputReadyNs - frame.queueDoneNs) / 1000000L;
                totalQueueToOutputMs += queueToOutputMs;
                maxQueueToOutputMs = Math.max(maxQueueToOutputMs, queueToOutputMs);
                queueToOutputMsValues.add(queueToOutputMs);
                int depth = queuedFrames - outputFrames;
                totalDepthAtOutput += depth;
                maxDepthAtOutput = Math.max(maxDepthAtOutput, depth);
                addDepth(depthAtOutputBuckets, depth);
            }

            void recordRelease(long ptsUs, long releaseStartNs, long releaseDoneNs) {
                FrameStats frame = framesByPts.remove(ptsUs);
                if (frame == null) {
                    return;
                }
                ++releasedFrames;
                frame.releaseStartNs = releaseStartNs;
                frame.releaseDoneNs = releaseDoneNs;
                long releaseCallUs = (releaseDoneNs - releaseStartNs) / 1000L;
                totalReleaseCallUs += releaseCallUs;
                maxReleaseCallUs = Math.max(maxReleaseCallUs, releaseCallUs);
                if (frame.outputReadyNs > 0) {
                    long outputToReleaseMs = (releaseStartNs - frame.outputReadyNs) / 1000000L;
                    totalOutputToReleaseMs += outputToReleaseMs;
                    maxOutputToReleaseMs = Math.max(maxOutputToReleaseMs, outputToReleaseMs);
                }
            }

            String summary() {
                return "statsFrames recv=" + receivedFrames
                        + " queued=" + queuedFrames
                        + " output=" + outputFrames
                        + " released=" + releasedFrames
                        + " avgEncToRecvMs=" + avg(totalEncodedToReceiveMs, receivedFrames)
                        + " maxEncToRecvMs=" + maxEncodedToReceiveMs
                        + " avgPcSendMs=" + avg(totalPcSendMs, receivedFrames)
                        + " maxPcSendMs=" + maxPcSendMs
                        + " avgFlowReadMs=" + avg(totalFlowReadMs, receivedFrames)
                        + " maxFlowReadMs=" + maxFlowReadMs
                        + " avgQueueWaitMs=" + avg(totalQueueWaitMs, queuedFrames)
                        + " maxQueueWaitMs=" + maxQueueWaitMs
                        + " avgQueueCallMs=" + avg(totalQueueCallMs, queuedFrames)
                        + " maxQueueCallMs=" + maxQueueCallMs
                        + " avgQueueToOutputMs=" + avg(totalQueueToOutputMs, outputFrames)
                        + " p50QueueToOutputMs=" + percentile(queueToOutputMsValues, 50)
                        + " p95QueueToOutputMs=" + percentile(queueToOutputMsValues, 95)
                        + " maxQueueToOutputMs=" + maxQueueToOutputMs
                        + " avgOutputToReleaseMs=" + avg(totalOutputToReleaseMs, releasedFrames)
                        + " maxOutputToReleaseMs=" + maxOutputToReleaseMs
                        + " avgReleaseCallUs=" + avg(totalReleaseCallUs, releasedFrames)
                        + " maxReleaseCallUs=" + maxReleaseCallUs
                        + " avgDepthAfterQueue=" + avg(totalDepthAfterQueue, queuedFrames)
                        + " maxDepthAfterQueue=" + maxDepthAfterQueue
                        + " depthAfterQueueBuckets=" + bucketsToString(depthAfterQueueBuckets)
                        + " avgDepthAtOutput=" + avg(totalDepthAtOutput, outputFrames)
                        + " maxDepthAtOutput=" + maxDepthAtOutput
                        + " depthAtOutputBuckets=" + bucketsToString(depthAtOutputBuckets);
            }

            private long avg(long total, int count) {
                return count > 0 ? total / count : 0;
            }

            private FrameStats frameForPts(long ptsUs) {
                FrameStats frame = framesByPts.get(ptsUs);
                if (frame == null) {
                    frame = new FrameStats();
                    framesByPts.put(ptsUs, frame);
                }
                return frame;
            }

            private void addDepth(int[] buckets, int depth) {
                int index = depth;
                if (index < 0) {
                    index = 0;
                } else if (index >= buckets.length) {
                    index = buckets.length - 1;
                }
                ++buckets[index];
            }

            private String bucketsToString(int[] buckets) {
                StringBuilder builder = new StringBuilder();
                builder.append("[");
                for (int i = 0; i < buckets.length; ++i) {
                    if (i > 0) {
                        builder.append(",");
                    }
                    if (i == buckets.length - 1) {
                        builder.append(i).append("+");
                    } else {
                        builder.append(i);
                    }
                    builder.append(":").append(buckets[i]);
                }
                builder.append("]");
                return builder.toString();
            }

            private long percentile(ArrayList<Long> values, int percentile) {
                if (values.isEmpty()) {
                    return 0;
                }
                ArrayList<Long> sorted = new ArrayList<Long>(values);
                Collections.sort(sorted);
                int index = (int) Math.ceil((percentile / 100.0) * sorted.size()) - 1;
                if (index < 0) {
                    index = 0;
                } else if (index >= sorted.size()) {
                    index = sorted.size() - 1;
                }
                return sorted.get(index);
            }

            private static final class FrameStats {
                long socketFirstByteNs;
                long socketLastByteNs;
                long queueStartNs;
                long inputReadyNs;
                long queueDoneNs;
                long outputReadyNs;
                long releaseStartNs;
                long releaseDoneNs;
            }
        }
    }
}
