#include "WaveMixer.h"
#include "tjsCommHead.h"

#ifdef __APPLE__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <TargetConditionals.h>
#else

#include <AL/alc.h>
#include <AL/alext.h>

#endif
#ifdef __ANDROID__

#include "oboe/Oboe.h"

#endif

#include "DebugIntf.h"
#include "Platform.h"
#include "SysInitIntf.h"
#include "TickCount.h"
#include "WaveImpl.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <assert.h>
#include <iomanip>
#include <math.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string.h>
#include <unordered_set>

class iTVPAudioRenderer;

static iTVPAudioRenderer *TVPAudioRenderer;
static ALCcontext *TVPALContext = nullptr;

// Converts a KiriKiri wave format into the SDL3 audio spec used for
// stream-side conversion. Returns false for unsupported bit depths.
static bool WaveFormatToAudioSpec(const tTVPWaveFormat &fmt,
                                  SDL_AudioSpec &out_spec) {
    memset(&out_spec, 0, sizeof(out_spec));
    out_spec.freq = fmt.SamplesPerSec;
    out_spec.channels = fmt.Channels;
    if(fmt.IsFloat) {
        out_spec.format = SDL_AUDIO_F32;
    } else {
        switch(fmt.BitsPerSample) {
            case 8:
                out_spec.format = SDL_AUDIO_S8;
                break;
            case 16:
                out_spec.format = SDL_AUDIO_S16;
                break;
            case 32:
                out_spec.format = SDL_AUDIO_S32;
                break;
            default:
                return false;
        }
    }
    return true;
}

static SDL_AudioSpec WaveFormatToAudioSpec(const tTVPWaveFormat &fmt) {
    SDL_AudioSpec spec;
    if(!WaveFormatToAudioSpec(fmt, spec))
        memset(&spec, 0, sizeof(spec));
    return spec;
}

static void TVPEnsureALContext() {
    if(TVPALContext && alcGetCurrentContext() != TVPALContext)
        alcMakeContextCurrent(TVPALContext);
}

template <int ch>
void MixAudioS16CPP(void *dst, const void *src, int samples, int16_t *volume) {
    int16_t *dst16 = (int16_t *)dst;
    const int16_t *src16 = (const int16_t *)src;
    while(samples--) {
        for(int i = 0; i < ch; ++i) {
            int src_sample = *src16++;
            src_sample = (src_sample * volume[i]) >> 14;
            int dest_sample = *dst16;
            if(src_sample > 0 && dest_sample > 0) {
                dest_sample = src_sample + dest_sample -
                    ((dest_sample * src_sample + 0x8000) >> 15);
            } else if(src_sample < 0 && dest_sample < 0) {
                dest_sample = src_sample + dest_sample +
                    ((dest_sample * src_sample) >> 15);
            } else {
                dest_sample += src_sample;
            }
            *dst16++ = dest_sample;
        }
    }
}

template <int ch>
void MixAudioF32CPP(void *dst, const void *src, int samples, int16_t *volume) {
    float *dst32 = (float *)dst;
    const float *src32 = (const float *)src;
    const float fmaxvolume = 1.0f / 16384 /*tTVPSoundBuffer::MAX_VOLUME*/;
    float fvolume[ch];
    for(int i = 0; i < ch; ++i)
        fvolume[i] = volume[i] * fmaxvolume;
    while(samples--) {
        for(int i = 0; i < ch; ++i) {
            float src_sample = SDL_SwapFloatLE(*src32++) * fvolume[i];
            float dest_sample = SDL_SwapFloatLE(*dst32);
            if(src_sample > 0 && dest_sample > 0) {
                dest_sample =
                    src_sample + dest_sample - dest_sample * src_sample;
            } else if(src_sample < 0 && dest_sample < 0) {
                dest_sample =
                    src_sample + dest_sample + dest_sample * src_sample;
            } else {
                dest_sample += src_sample;
            }
            *(dst32++) = SDL_SwapFloatLE(dest_sample);
        }
    }
}

typedef void(FAudioMix)(void *dst, const void *src, int samples,
                        int16_t *volume);

static FAudioMix *_AudioMixS16[8] = { // 7.1 max
    &MixAudioS16CPP<1>, &MixAudioS16CPP<2>, &MixAudioS16CPP<3>,
    &MixAudioS16CPP<4>, &MixAudioS16CPP<5>, &MixAudioS16CPP<6>,
    &MixAudioS16CPP<7>, &MixAudioS16CPP<8>
};
static FAudioMix *_AudioMixF32[8] = { // 7.1 max
    &MixAudioF32CPP<1>, &MixAudioF32CPP<2>, &MixAudioF32CPP<3>,
    &MixAudioF32CPP<4>, &MixAudioF32CPP<5>, &MixAudioF32CPP<6>,
    &MixAudioF32CPP<7>, &MixAudioF32CPP<8>
};

extern "C" void TVPWaveMixer_ASM_Init(FAudioMix **func16, FAudioMix **func32);

class tTVPSoundBuffer : public iTVPSoundBuffer {
public:
    bool _playing = false;
    float _volume = 1;
    float _pan = 0;
    const signed int MAX_VOLUME = 16384; // limit in signed 16bit
    int16_t _volume_raw[8];
    bool _needs_convert = false;
    SDL_AudioSpec _src_spec{};
    SDL_AudioSpec _dst_spec{};
    int _frame_size = 0;
    int _input_frame_size = 0;

    void RecalcVolume() {
        if(_pan > 0) {
            _volume_raw[0] = (1.0f - _pan) * _volume * MAX_VOLUME;
        } else {
            _volume_raw[0] = _volume * MAX_VOLUME;
        }
        if(_pan < 0) {
            _volume_raw[1] = (_pan + 1.0f) * _volume * MAX_VOLUME;
        } else {
            _volume_raw[1] = _volume * MAX_VOLUME;
        }
        _volume_raw[2] = _volume_raw[0]; // for SIMD
        _volume_raw[3] = _volume_raw[1];
    }

    std::mutex _buffer_mtx;
    std::deque<std::vector<uint8_t>> _buffers;
    tjs_uint _sendedFrontBuffer = 0;
    tjs_uint _sendedSamples = 0, _inCachedSamples = 0;

    tTVPSoundBuffer(const SDL_AudioSpec &src_spec, const SDL_AudioSpec &dst_spec) :
        _src_spec(src_spec), _dst_spec(dst_spec) {
        RecalcVolume();
        _needs_convert =
            src_spec.freq != dst_spec.freq ||
            src_spec.channels != dst_spec.channels ||
            src_spec.format != dst_spec.format;
        _frame_size =
            SDL_AUDIO_BITSIZE(dst_spec.format) / 8 * dst_spec.channels;
        _input_frame_size =
            SDL_AUDIO_BITSIZE(src_spec.format) / 8 * src_spec.channels;
    }

    ~tTVPSoundBuffer() override;

    void Release() override { delete this; }

    void Play() override { _playing = true; }

    void Pause() override { _playing = false; }

    void Stop() override {
        _playing = false;
        Reset();
    }

    void Reset() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        _buffers.clear();
        _inCachedSamples = 0;
        _sendedFrontBuffer = 0;
        _sendedSamples = 0;
    }

    bool IsPlaying() override { return _playing; }

    void SetVolume(float v) override {
        _volume = v;
        RecalcVolume();
    }

    float GetVolume() override { return _volume; }

    void SetPan(float v) override {
        _pan = v;
        RecalcVolume();
    }

    float GetPan() override { return _pan; }

    void AppendBuffer(const void *_inbuf,
                      unsigned int inlen /*, int tag = 0*/) override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        if(_needs_convert) {
            Uint8 *converted = nullptr;
            int converted_len = 0;
            if(SDL_ConvertAudioSamples(&_src_spec, (const Uint8 *)_inbuf,
                                       (int)inlen, &_dst_spec, &converted,
                                       &converted_len) != 0) {
                SDL_Log("WaveMixer: convert failed: %s", SDL_GetError());
                return;
            }
            _inCachedSamples += converted_len / _frame_size;
            _buffers.emplace_back(converted, converted + converted_len);
            SDL_free(converted);
        } else {
            _buffers.emplace_back((uint8_t *)_inbuf,
                                  ((uint8_t *)_inbuf) + inlen);
            _inCachedSamples += inlen / _frame_size;
        }
    }

    bool IsBufferValid() override {
        return true; // unlimited buffer size
                     // return !_buffers.empty(); // thread safe if
                     // read only
    }

    tjs_uint GetLatencySamples() override;

    // 	virtual void SetSampleOffset(tjs_uint n) override {
    // 		_sendedSamples = n;
    // 	}
    int GetRemainBuffers() override { return _buffers.size(); }

    tjs_uint GetCurrentPlaySamples() override;

    tjs_uint GetPlaybackSampleRate() override;

    float GetLatencySeconds() override;

    void FillBuffer(uint8_t *out, int len);
};

class iTVPAudioRenderer {
public:
    SDL_AudioSpec _spec;
    std::mutex _streams_mtx;
    std::unordered_set<tTVPSoundBuffer *> _streams;
    int _frame_size = 0;
    std::atomic<std::uint64_t> _callback_count{0};
    std::atomic<Uint64> _last_callback_ms{0};
    // Stream input bytes needed per device output byte (format conversion).
    // The stream callback's total_amount is expressed in the device format,
    // while FillBuffer mixes in the requested input format.
    double _convert_ratio = 1.0;

    iTVPAudioRenderer() {
        memset(&_spec, 0, sizeof(_spec));
        _spec.freq = 48000;
        _spec.format = SDL_AUDIO_S16;
        _spec.channels = 2;
        _frame_size = 4;
    }
    virtual ~iTVPAudioRenderer() = default;

    void InitMixer() {
        if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) { // for format converter
            SDL_Log("Fail to initialize audio.");
            return;
        }
    }

    FAudioMix *DoMixAudio;

    void SetupMixer() {
        if(_spec.format == SDL_AUDIO_S16) {
            DoMixAudio = _AudioMixS16[_spec.channels - 1];
        } else if(_spec.format == SDL_AUDIO_F32) {
            DoMixAudio = _AudioMixF32[_spec.channels - 1];
        } else {
            DoMixAudio = [](void *dst, const void *src, int samples,
                            int16_t *volume) {};
        }
    }

    virtual bool Init() = 0;

    virtual void SuspendForHost() {}

    virtual bool ResumeForHost() { return true; }

    virtual bool IsSuspendedForHost() const { return false; }

    virtual void PollForHost() {}

    virtual tTVPSoundBuffer *CreateStream(tTVPWaveFormat &fmt, int bufcount) {
        SDL_AudioSpec src_spec;
        if(!WaveFormatToAudioSpec(fmt, src_spec))
            return nullptr;
        SDL_AudioSpec dst_spec = _spec;
        tTVPSoundBuffer *s = new tTVPSoundBuffer(src_spec, dst_spec);
        std::lock_guard<std::mutex> lk(_streams_mtx);
        _streams.emplace(s);
        return s;
    }

    void ReleaseStream(tTVPSoundBuffer *s) {
        std::lock_guard<std::mutex> lk(_streams_mtx);
        _streams.erase(s);
    }

    void FillBuffer(Uint8 *buf, int len) {
        // memset(buf, 0, len);
        std::lock_guard<std::mutex> lk(_streams_mtx);
        for(tTVPSoundBuffer *s : _streams) {
            s->FillBuffer(buf, len);
        }
    }

    int MixAudio(uint8_t *dst, uint8_t *src, int len, int16_t *vol) {
        int samples = len / _frame_size;
        DoMixAudio(dst, src, samples, vol);
        return samples;
    }

    const SDL_AudioSpec &GetSpec() { return _spec; }

    virtual int32_t GetUnprocessedSamples() { return 0; }
};

tTVPSoundBuffer::~tTVPSoundBuffer() {
    Stop();
    TVPAudioRenderer->ReleaseStream(this);
}

tjs_uint tTVPSoundBuffer::GetLatencySamples() {
    std::lock_guard<std::mutex> lk(_buffer_mtx);
    int32_t samples = TVPAudioRenderer->GetUnprocessedSamples();
    return static_cast<tjs_uint>(std::max<int32_t>(samples, 0)) +
        _inCachedSamples;
}

tjs_uint tTVPSoundBuffer::GetCurrentPlaySamples() {
    std::lock_guard<std::mutex> lk(_buffer_mtx);
    int32_t samples = TVPAudioRenderer->GetUnprocessedSamples();
    if(samples > _sendedSamples)
        return 0;
    return _sendedSamples - samples; // -GetLatencySamples();
}

tjs_uint tTVPSoundBuffer::GetPlaybackSampleRate() {
    return static_cast<tjs_uint>(
        std::max(TVPAudioRenderer->GetSpec().freq, 0));
}

float tTVPSoundBuffer::GetLatencySeconds() {
    const int sample_rate = TVPAudioRenderer->GetSpec().freq;
    if(sample_rate <= 0)
        return 0.0f;
    return static_cast<float>(GetLatencySamples()) /
        static_cast<float>(sample_rate);
}

void tTVPSoundBuffer::FillBuffer(uint8_t *out, int len) {
    if(!_playing)
        return;
    std::lock_guard<std::mutex> lk(_buffer_mtx);
    while(len > 0 && !_buffers.empty()) {
        std::vector<uint8_t> &buf = _buffers.front();
        if(buf.size() > _sendedFrontBuffer) {
            int n = std::min((size_t)len, buf.size() - _sendedFrontBuffer);
            int samples = TVPAudioRenderer->MixAudio(
                out, &buf.front() + _sendedFrontBuffer, n, _volume_raw);
            _sendedSamples += samples;
            _inCachedSamples -= samples;
            _sendedFrontBuffer += n;
            out += n;
            len -= n;
        }
        if(_sendedFrontBuffer >= buf.size()) {
            _sendedFrontBuffer = 0;
            _buffers.pop_front();
        }
    }
}

class tTVPAudioRendererSDL : public iTVPAudioRenderer {
    SDL_AudioStream *_stream = nullptr;
    bool _host_suspended = false;
    Uint64 _next_resume_attempt_ms = 0;
    bool _resume_probe_pending = false;
    Uint64 _resume_probe_started_ms = 0;
    std::uint64_t _resume_probe_callback_count = 0;

    // SDL3 audio stream callback: invoked on the device thread when the
    // stream needs data. Mix the requested amount and push it into the
    // stream; SDL converts to the device format when they differ.
    static void SDLCALL StreamCallback(void *userdata, SDL_AudioStream *stream,
                                       int additional_amount,
                                       int total_amount) {
        auto *renderer = static_cast<iTVPAudioRenderer *>(userdata);
        renderer->_callback_count.fetch_add(1, std::memory_order_relaxed);
        renderer->_last_callback_ms.store(SDL_GetTicks(),
                                          std::memory_order_relaxed);
        if(total_amount <= 0)
            return;
        int put = static_cast<int>(total_amount * renderer->_convert_ratio);
        const int frame = renderer->_frame_size;
        if(frame > 1)
            put -= put % frame;
        if(put <= 0)
            put = frame;
        std::vector<uint8_t> scratch(static_cast<size_t>(put));
        memset(scratch.data(), 0, scratch.size());
        renderer->FillBuffer(scratch.data(), put);
        SDL_PutAudioStreamData(stream, scratch.data(), put);
    }

    void LogLifecycleState(const char *event) const {
        const Uint64 now = SDL_GetTicks();
        const Uint64 last = _last_callback_ms.load(std::memory_order_relaxed);
        const auto callbacks =
            _callback_count.load(std::memory_order_relaxed);
        spdlog::info(
            "iOS audio lifecycle {} stream={} suspended={} "
            "callbacks={} last_callback_age_ms={} sdl_error=\"{}\"",
            event, _stream ? 1 : 0, _host_suspended ? 1 : 0,
            static_cast<unsigned long long>(callbacks),
            last == 0 || now < last
                ? static_cast<unsigned long long>(0)
                : static_cast<unsigned long long>(now - last),
            SDL_GetError());
    }

    bool OpenPlaybackDevice() {
        _stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &_spec, StreamCallback, this);
        if(!_stream) {
            SDL_Log("Fail to open audio @%dHz: %s", _spec.freq,
                    SDL_GetError());
            return false;
        }
        _frame_size = SDL_AUDIO_BITSIZE(_spec.format) / 8 * _spec.channels;
        _convert_ratio = 1.0;
        const SDL_AudioDeviceID device_id =
            SDL_GetAudioStreamDevice(_stream);
        if(device_id != 0) {
            SDL_AudioSpec device_spec;
            int sample_frames = 0;
            if(SDL_GetAudioDeviceFormat(device_id, &device_spec,
                                        &sample_frames)) {
                const double in_rate =
                    (double)_spec.freq * SDL_AUDIO_BITSIZE(_spec.format) *
                    _spec.channels;
                const double out_rate =
                    (double)device_spec.freq *
                    SDL_AUDIO_BITSIZE(device_spec.format) *
                    device_spec.channels;
                if(out_rate > 0.0)
                    _convert_ratio = in_rate / out_rate;
            }
        }
        SDL_Log("Audio Device: %s", SDL_GetCurrentAudioDriver());
        SDL_ResumeAudioStreamDevice(_stream);
        SetupMixer();
        return true;
    }

public:
    virtual ~tTVPAudioRendererSDL() {
        if(_stream)
            SDL_DestroyAudioStream(_stream);
    }

    bool Init() override {
        InitMixer();
        return OpenPlaybackDevice();
    }

    void SuspendForHost() override {
        if(_host_suspended)
            return;
        LogLifecycleState("suspend_begin");
        _host_suspended = true;
        _next_resume_attempt_ms = 0;
        _resume_probe_pending = false;
        if(_stream) {
            SDL_PauseAudioStreamDevice(_stream);
        }
        LogLifecycleState("suspend_complete");
    }

    bool ResumeForHost() override {
        if(!_host_suspended)
            return true;

#if defined(__APPLE__) && TARGET_OS_IPHONE
        const Uint64 now = SDL_GetTicks();
        if(_next_resume_attempt_ms != 0 && now < _next_resume_attempt_ms)
            return false;

        LogLifecycleState("resume_attempt");

        // SDL's iOS backend owns an interruption observer for this device. Keep
        // it alive across suspension so UIApplicationDidBecomeActive can
        // restart its AudioQueue, then explicitly reactivate the shared
        // AVAudioSession. Destroying the stream here removes that observer and
        // can also deactivate Godot's audio session out from under the host.
        if(!TVPActivateAudioSessionForHost()) {
            _next_resume_attempt_ms = now + 250;
            LogLifecycleState("resume_session_deferred");
            return false;
        }
#endif
        if(_stream)
            SDL_ResumeAudioStreamDevice(_stream);
        _host_suspended = false;
        _next_resume_attempt_ms = 0;
        _resume_probe_pending = true;
        _resume_probe_started_ms = SDL_GetTicks();
        _resume_probe_callback_count =
            _callback_count.load(std::memory_order_relaxed);
        LogLifecycleState("resume_unpaused");
        return true;
    }

    bool IsSuspendedForHost() const override { return _host_suspended; }

    void PollForHost() override {
        if(!_resume_probe_pending)
            return;
        const Uint64 now = SDL_GetTicks();
        const auto callbacks =
            _callback_count.load(std::memory_order_relaxed);
        if(callbacks > _resume_probe_callback_count) {
            _resume_probe_pending = false;
            LogLifecycleState("resume_callback_healthy");
            return;
        }
        if(now - _resume_probe_started_ms >= 2000) {
            _resume_probe_pending = false;
            LogLifecycleState("resume_callback_stalled");
        }
    }
};

#ifdef __ANDROID__

class tTVPAudioRendererOboe : public iTVPAudioRenderer,
                              public oboe::AudioStreamCallback {
    oboe::AudioStream *_oboeAudioStream = nullptr;

public:
    virtual ~tTVPAudioRendererOboe() {
        if(_oboeAudioStream)
            delete _oboeAudioStream;
    }

    bool Init() override {
        InitMixer();
        // Create a builder
        oboe::AudioStreamBuilder builder;
        // builder.setFormat(oboe::AudioFormat::I16);
        builder.setChannelCount(2);
        // builder.setSampleRate(oboe::kUnspecified);
        builder.setCallback(this);
        // 	builder.setPerformanceMode(PerformanceMode::None);
        // 	builder.setSharingMode(SharingMode::Shared);
        oboe::Result result = builder.openStream(&_oboeAudioStream);
        // 		if (result != oboe::Result::OK) {
        // 			// try down sample rate
        // 			_spec.freq = 44100;
        // 			builder.setSampleRate(_spec.freq);
        // 			result = builder.openStream(&_oboeAudioStream);
        // 		}
        if(result == oboe::Result::OK) {
            _spec.freq = _oboeAudioStream->getSampleRate();
            switch(_oboeAudioStream->getFormat()) {
                case oboe::AudioFormat::I16:
                    _spec.format = SDL_AUDIO_S16;
                    break;
                case oboe::AudioFormat::Float:
                    _spec.format = SDL_AUDIO_F32;
                    break;
                default:
                    break;
            }
            _frame_size = SDL_AUDIO_BITSIZE(_spec.format) / 8 * _spec.channels;
            _oboeAudioStream->requestStart();
            SDL_Log("Audio Device: Oboe @%dHz", _spec.freq);
            SetupMixer();
            return true;
        }
        SDL_Log("Fail to open Oboe audio");
        // SetupSDL();
        return false;
    }

    virtual oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream,
                                                  void *audioData,
                                                  int32_t numFrames) override {
        int len = _frame_size * numFrames;
        memset(audioData, 0, _frame_size * numFrames);
        // if (oboeStream == _oboeAudioStream)
        FillBuffer((uint8_t *)audioData, len);
        // 		else
        // 			fillCaptureBuffer((uint8_t*)audioData, /*Mono*/2
        // * numFrames);
        return oboe::DataCallbackResult::Continue;
    }

    virtual int32_t GetUnprocessedSamples() {
        int64_t hardwareFrameIndex;
        int64_t timeNanoseconds;
        oboe::Result result = _oboeAudioStream->getTimestamp(
            CLOCK_MONOTONIC, &hardwareFrameIndex, &timeNanoseconds);
        if(result != oboe::Result::OK) { // OpenSL TODO accumulate calc
            return 0;
        }
        int64_t appFrameIndex = _oboeAudioStream->getFramesWritten();
        return appFrameIndex - hardwareFrameIndex;
    }
};

#endif

class tTVPSoundBufferAL : public tTVPSoundBuffer {
    typedef tTVPSoundBuffer inherit;

    ALuint _alSource;
    ALenum _alFormat;
    ALuint *_bufferIds, *_bufferIds2;
    tjs_uint *_bufferSize;
    tjs_uint _bufferCount;
    int _bufferIdx = -1;
    tTVPWaveFormat _format;

public:
    tTVPSoundBufferAL(tTVPWaveFormat &desired, int bufcount) :
        tTVPSoundBuffer(WaveFormatToAudioSpec(desired),
                        WaveFormatToAudioSpec(desired)),
        _bufferCount(bufcount) {
        if(_frame_size <= 0) {
            _frame_size = desired.BytesPerSample * desired.Channels;
            _input_frame_size = _frame_size;
        }
        _bufferIds = new ALuint[bufcount];
        _bufferIds2 = new ALuint[bufcount];
        _bufferSize = new tjs_uint[bufcount];
        std::fill(_bufferSize, _bufferSize + _bufferCount, 0);
        _format = desired;
        TVPEnsureALContext();
        alGenSources(1, &_alSource);
        alGenBuffers(_bufferCount, _bufferIds);
        alSourcef(_alSource, AL_GAIN, 1.0f);
        if(desired.Channels == 1) {
            switch(desired.BitsPerSample) {
                case 8:
                    _alFormat = AL_FORMAT_MONO8;
                    break;
                case 16:
                    _alFormat = AL_FORMAT_MONO16;
                    break;
                default:
                    assert(false);
            }
        } else if(desired.Channels == 2) {
            switch(desired.BitsPerSample) {
                case 8:
                    _alFormat = AL_FORMAT_STEREO8;
                    break;
                case 16:
                    _alFormat = AL_FORMAT_STEREO16;
                    break;
                default:
                    assert(false);
            }
        } else {
            assert(false);
        }
    }

    ~tTVPSoundBufferAL() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        TVPEnsureALContext();
        if(_alSource) {
            alSourceStop(_alSource);
            UnqueueAllBuffersLocked();
            alSourcei(_alSource, AL_BUFFER, 0);
            alDeleteSources(1, &_alSource);
            _alSource = 0;
        }
        if(_bufferIds)
            alDeleteBuffers(_bufferCount, _bufferIds);
        delete[] _bufferIds;
        delete[] _bufferIds2;
        delete[] _bufferSize;
    }

    bool IsBufferValid() override {
        TVPEnsureALContext();
        ALint processed = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        if(processed > 0)
            return true;
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        return queued < _bufferCount;
    }

    void AppendBuffer(const void *buf,
                      unsigned int len /*, int tag = 0*/) override {
        if(len <= 0)
            return;
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        TVPEnsureALContext();

        /* First remove any processed buffers. */
        ALint processed = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        if(processed > 0) {
            if(processed > static_cast<ALint>(_bufferCount))
                processed = _bufferCount;
            alSourceUnqueueBuffers(_alSource, processed, _bufferIds2);
            checkerr("alSourceUnqueueBuffers");
            for(int i = 0; i < processed; ++i) {
                for(int j = 0; j < _bufferCount; ++j) {
                    if(_bufferIds[j] == _bufferIds2[i]) {
                        _sendedSamples += _bufferSize[j] / _frame_size;
                        break;
                    }
                }
            }
        }

        /* Refill the buffer queue. */
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);

        if(queued >= _bufferCount)
            return;
        ++_bufferIdx;
        if(_bufferIdx >= _bufferCount)
            _bufferIdx = 0;
        ALuint bufid = _bufferIds[_bufferIdx];
        alBufferData(bufid, _alFormat, buf, len, _format.SamplesPerSec);
        checkerr("alBufferData");
        alSourceQueueBuffers(_alSource, 1, &bufid);
        checkerr("alSourceQueueBuffers");
        //_tags[_bufferIdx] = tag;
        _bufferSize[_bufferIdx] = len;
        if(_playing) {
            ALenum state;
            alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
            checkerr("AppendBuffer state");
            if(state != AL_PLAYING) {
                alSourcePlay(_alSource);
                checkerr("AppendBuffer play");
            }
        }
    }

    void Reset() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        _buffers.clear();
        _inCachedSamples = 0;
        _sendedFrontBuffer = 0;
        _sendedSamples = 0;
        TVPEnsureALContext();
        if(_alSource) {
            alSourceStop(_alSource);
            UnqueueAllBuffersLocked();
            alSourcei(_alSource, AL_BUFFER, 0);
            alSourceRewind(_alSource);
            checkerr("Reset rewind");
        }
        _playing = false;
        _bufferIdx = -1;
        std::fill(_bufferSize, _bufferSize + _bufferCount, 0);
    }

    void Pause() override {
        TVPEnsureALContext();
        alSourcePause(_alSource);
        checkerr("Pause");
        _playing = false;
    }

    static void checkerr(const char *funcname);

    void Play() override {
        TVPEnsureALContext();
        ALenum state;
        alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        checkerr("Play");
        if(state != AL_PLAYING) {
            alSourcePlay(_alSource);
            checkerr("Play");
        }

        _playing = true;
    }

    void Stop() override {
        TVPEnsureALContext();
        alSourceStop(_alSource);
        checkerr("Stop");
        Reset();
        _playing = false;
    }

    void SetVolume(float volume) override {
        TVPEnsureALContext();
        alSourcef(_alSource, AL_GAIN, volume);
        checkerr("SetVolume");
    }

    float GetVolume() override {
        TVPEnsureALContext();
        float volume = 0;
        alGetSourcef(_alSource, AL_GAIN, &volume);
        return volume;
    }

    void SetPan(float pan) override {
        TVPEnsureALContext();
        float sourcePosAL[] = { pan, 0.0f, 0.0f };
        alSourcefv(_alSource, AL_POSITION, sourcePosAL);
    }

    float GetPan() override {
        TVPEnsureALContext();
        float sourcePosAL[3];
        alGetSourcefv(_alSource, AL_POSITION, sourcePosAL);
        return sourcePosAL[0];
    }

    bool IsPlaying() override {
        TVPEnsureALContext();
        ALenum state;
        alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        return state == AL_PLAYING;
    }

    void SetPosition(float x, float y, float z) override {
        TVPEnsureALContext();
        float sourcePosAL[] = { x, y, z };
        alSourcefv(_alSource, AL_POSITION, sourcePosAL);
        checkerr("SetPosition");
    }

    int GetRemainBuffers() override {
        TVPEnsureALContext();
        ALint processed, queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        return queued - processed;
    }

    tjs_uint GetLatencySamples() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        TVPEnsureALContext();
        ALint offset = 0, queued = 0;
        alGetSourcei(_alSource, AL_BYTE_OFFSET, &offset);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        int remainBuffers = queued;
        if(remainBuffers == 0)
            return 0;
        tjs_int total = -offset;
        for(int i = 0; i < remainBuffers; ++i) {
            int idx = _bufferIdx + 1 - remainBuffers + i;
            if(idx >= _bufferCount)
                idx -= _bufferCount;
            else if(idx < 0)
                idx += _bufferCount;
            total += _bufferSize[idx];
        }
        return total / _frame_size;
    }

    float GetLatencySeconds() override {
        return (float)GetLatencySamples() / _format.SamplesPerSec;
    }

    tjs_uint GetCurrentPlaySamples() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        TVPEnsureALContext();
        ALint offset = 0;
        alGetSourcei(_alSource, AL_SAMPLE_OFFSET, &offset);
        return _sendedSamples + offset;
    }

    tjs_uint GetPlaybackSampleRate() override {
        return static_cast<tjs_uint>(_format.SamplesPerSec);
    }

private:
    void UnqueueAllBuffersLocked() {
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        while(queued-- > 0) {
            ALuint bufid = 0;
            alSourceUnqueueBuffers(_alSource, 1, &bufid);
        }
    }
};

class tTVPAudioRendererAL : public iTVPAudioRenderer {
    using ALCDevicePauseProc = void(ALC_APIENTRY *)(ALCdevice *);
    using ALCDeviceResumeProc = void(ALC_APIENTRY *)(ALCdevice *);
    using ALCReopenDeviceProc = ALCboolean(ALC_APIENTRY *)(
        ALCdevice *, const ALCchar *, const ALCint *);

    ALCdevice *_device = nullptr;
    ALCcontext *_context = nullptr;
    bool _host_suspended = false;
    Uint64 _next_resume_attempt_ms = 0;
    bool _used_device_pause = false;
    bool _resume_probe_pending = false;
    Uint64 _resume_probe_started_ms = 0;
    ALCDevicePauseProc _device_pause = nullptr;
    ALCDeviceResumeProc _device_resume = nullptr;
    ALCReopenDeviceProc _reopen_device = nullptr;

    void LoadHostLifecycleExtensions() {
        if(!_device)
            return;
        if(alcIsExtensionPresent(_device, "ALC_SOFT_pause_device") ==
           ALC_TRUE) {
            _device_pause = reinterpret_cast<ALCDevicePauseProc>(
                alcGetProcAddress(_device, "alcDevicePauseSOFT"));
            _device_resume = reinterpret_cast<ALCDeviceResumeProc>(
                alcGetProcAddress(_device, "alcDeviceResumeSOFT"));
        }
        if(alcIsExtensionPresent(_device, "ALC_SOFT_reopen_device") ==
           ALC_TRUE) {
            _reopen_device = reinterpret_cast<ALCReopenDeviceProc>(
                alcGetProcAddress(_device, "alcReopenDeviceSOFT"));
        }
        spdlog::info(
            "iOS OpenAL host extensions pause_device={} reopen_device={}",
            _device_pause && _device_resume ? 1 : 0,
            _reopen_device ? 1 : 0);
    }

    void LogLifecycleState(const char *event) {
        std::size_t stream_count = 0;
        std::size_t logical_playing = 0;
        std::size_t source_playing = 0;
        {
            std::lock_guard<std::mutex> lk(_streams_mtx);
            stream_count = _streams.size();
            for(tTVPSoundBuffer *stream : _streams) {
                if(!stream->_playing)
                    continue;
                ++logical_playing;
                // Querying a source implicitly restores TVPALContext as the
                // current context. Leave it detached while host-suspended.
                if(!_host_suspended && stream->IsPlaying())
                    ++source_playing;
            }
        }
        const ALCenum error = _device ? alcGetError(_device) : ALC_NO_ERROR;
        spdlog::info(
            "iOS OpenAL lifecycle {} device={} context={} context_current={} "
            "suspended={} streams={} logical_playing={} source_playing={} "
            "alc_error={}",
            event, static_cast<const void *>(_device),
            static_cast<const void *>(_context),
            _context && alcGetCurrentContext() == _context ? 1 : 0,
            _host_suspended ? 1 : 0, stream_count, logical_playing,
            source_playing, static_cast<int>(error));
    }

    void RestartLogicalStreams() {
        std::lock_guard<std::mutex> lk(_streams_mtx);
        for(tTVPSoundBuffer *stream : _streams) {
            if(stream->_playing && !stream->IsPlaying())
                stream->Play();
        }
    }

public:
    virtual ~tTVPAudioRendererAL() {
        if(_context) {
            // alDeleteSources(TVP_MAX_AUDIO_COUNT, _alSources);
            if(_context == TVPALContext)
                TVPALContext = nullptr;
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(_context);
        }
        if(_device)
            alcCloseDevice(_device);
    }

    bool Init() override {
        ALboolean enumeration =
            alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT");
        if(enumeration == AL_FALSE) {
            // enumeration not supported
            _device = alcOpenDevice(nullptr);
        } else {
            // enumeration supported
            const ALCchar *devices =
                alcGetString(nullptr, ALC_DEVICE_SPECIFIER);
            std::vector<std::string> alldev;
            ttstr log(TJS_W("(info) Sound Driver/Device found : "));
            while(*devices) {
                TVPAddImportantLog(log + devices);
                alldev.emplace_back(devices);
                devices += alldev.back().length() + 1;
            }
            _device =
                alcOpenDevice(alldev.empty() ? nullptr : alldev[0].c_str());
        }
        if(!_device)
            return false;

        _context = alcCreateContext(_device, nullptr);
        if(!_context)
            return false;
        alcMakeContextCurrent(_context);
        TVPALContext = _context;
        LoadHostLifecycleExtensions();

        return true;
    }

    tTVPSoundBuffer *CreateStream(tTVPWaveFormat &fmt, int bufcount) override {
        tTVPSoundBuffer *s = new tTVPSoundBufferAL(fmt, bufcount);
        std::lock_guard<std::mutex> lk(_streams_mtx);
        _streams.emplace(s);
        return s;
    }

    void SuspendForHost() override {
        if(_host_suspended)
            return;
        LogLifecycleState("suspend_begin");
        _host_suspended = true;
        _next_resume_attempt_ms = 0;
        _resume_probe_pending = false;
        if(_context) {
            // OpenAL Soft treats alcSuspendContext as deferred property
            // updates, not as a hardware/DSP pause. Stop the actual output
            // device before iOS deactivates AVAudioSession so its RemoteIO
            // unit has a clean foreground restart path.
            if(_device_pause && _device_resume) {
                _device_pause(_device);
                _used_device_pause = true;
            } else {
                alcSuspendContext(_context);
                _used_device_pause = false;
            }
            alcMakeContextCurrent(nullptr);
        }
        LogLifecycleState("suspend_complete");
    }

    bool ResumeForHost() override {
        if(!_host_suspended)
            return true;

#if defined(__APPLE__) && TARGET_OS_IPHONE
        const Uint64 now = SDL_GetTicks();
        if(_next_resume_attempt_ms != 0 && now < _next_resume_attempt_ms)
            return false;
        LogLifecycleState("resume_attempt");
        if(!TVPActivateAudioSessionForHost()) {
            _next_resume_attempt_ms = now + 250;
            LogLifecycleState("resume_session_deferred");
            return false;
        }
#endif

        // Recreate OpenAL Soft's CoreAudio RemoteIO backend after the shared
        // session has been reactivated. ALC_SOFT_reopen_device preserves all
        // contexts, sources, buffers, playback offsets, and object identity.
        // Merely processing the context cannot revive a RemoteIO unit stopped
        // by iOS suspension.
        if(_reopen_device) {
            const ALCboolean reopened =
                _reopen_device(_device, nullptr, nullptr);
            spdlog::info("iOS OpenAL device reopen result={}",
                         reopened == ALC_TRUE ? 1 : 0);
        }

        if(_context) {
            alcMakeContextCurrent(_context);
            if(_used_device_pause && _device_resume)
                _device_resume(_device);
            else
                alcProcessContext(_context);
        }
        TVPALContext = _context;
        RestartLogicalStreams();
        _host_suspended = false;
        _next_resume_attempt_ms = 0;
        _resume_probe_pending = true;
        _resume_probe_started_ms = SDL_GetTicks();
        LogLifecycleState("resume_complete");
        return true;
    }

    bool IsSuspendedForHost() const override { return _host_suspended; }

    void PollForHost() override {
        if(!_resume_probe_pending)
            return;
        const Uint64 now = SDL_GetTicks();
        if(now - _resume_probe_started_ms < 500)
            return;
        _resume_probe_pending = false;
        LogLifecycleState("resume_post_unlock");
    }

    ALCcontext *GetContext() { return _context; }
};

void tTVPSoundBufferAL::checkerr(const char *funcname) {
#if _DEBUG
    TVPEnsureALContext();
    ALenum err = alGetError();
    if(AL_NO_ERROR == err)
        return;
    SDL_Log("%s OpenAL Error %X", funcname, err);
#endif
}

static iTVPAudioRenderer *CreateAudioRenderer() {
    iTVPAudioRenderer *renderer = nullptr;
#ifdef __ANDROID__
    renderer = new tTVPAudioRendererOboe;
    if(renderer->Init())
        return renderer;
    delete renderer;
#elif defined(_MSC_VER) && 0
    renderer = new tTVPAudioRendererSDL;
    renderer->Init();
    return renderer;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    renderer = new tTVPAudioRendererSDL;
    if(renderer->Init()) {
        spdlog::info("iOS audio renderer selected: SDL");
        return renderer;
    }
    spdlog::warn("iOS SDL audio renderer unavailable; falling back to OpenAL");
    delete renderer;
#endif
    renderer = new tTVPAudioRendererAL;
    renderer->Init();
#if defined(__APPLE__) && TARGET_OS_IPHONE
    spdlog::info("iOS audio renderer selected: OpenAL");
#endif
    return renderer;
}

void TVPInitDirectSound(int freq) {
    if(!TVPAudioRenderer) {
        TVPAudioRenderer = CreateAudioRenderer();
    }
    // TVPInitSoundOptions();
}

void TVPUninitDirectSound() {
    delete TVPAudioRenderer;
    TVPAudioRenderer = nullptr;
    TVPALContext = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void TVPSuspendAudioRendererForHost() {
    if(TVPAudioRenderer)
        TVPAudioRenderer->SuspendForHost();
}

bool TVPResumeAudioRendererForHost() {
    return !TVPAudioRenderer || TVPAudioRenderer->ResumeForHost();
}

bool TVPIsAudioRendererSuspendedForHost() {
    return TVPAudioRenderer && TVPAudioRenderer->IsSuspendedForHost();
}

void TVPPollAudioRendererForHost() {
    if(TVPAudioRenderer)
        TVPAudioRenderer->PollForHost();
}

iTVPSoundBuffer *TVPCreateSoundBuffer(tTVPWaveFormat &fmt, int bufcount) {
    return TVPAudioRenderer->CreateStream(fmt, bufcount);
}

#if 0
int TVPALSoundWrap::GetNextBufferIndex() {
    int n = _bufferIdx + 1;
    if (n >= TVPAL_BUFFER_COUNT) n = 0;
    return n;
}

void TVPALSoundWrap::SetSampleOffset(tjs_uint n /*= 0*/) {
    _samplesProcessed = n;
}
#endif
