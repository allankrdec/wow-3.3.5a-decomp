// Stage E6 test: FMOD Ex through the CoreAudio output plugin (fmodex/mac/src/fmod_output_coreaudio.cpp).
//
//   SoundTest [game folder]       (the folder that contains Data/; without it the game-file part is skipped)
//
// 1. Initializes FMOD the way SoundEngine.cpp does (AUTODETECT output, driver 0, software channels, control panel
//    speaker mode, DSP buffer 1024 x 4, 3D right-handed) and checks that it picked the CoreAudio output.
// 2. Plays a generated tone, then a game .wav and a game .mp3 loaded through SFile, at volume 0.2.
// 3. Re-initializes with PCMFLOAT 44.1 kHz, 5.1 and 7.1 and plays the tone (panned left) in each.
// A render notify on the AudioUnit (System::getOutputHandle) counts the frames CoreAudio pulls and their peak level;
// each part passes only when CoreAudio rendered frames and they were not silent.
// Exit code 0 when everything passed.  Playback is short and quiet: about 12 seconds in total.

#include <AudioUnit/AudioUnit.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/SoundEngine/fmodex/src/fmod.hpp"
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_failures;

static bool Check(FMOD_RESULT result, const char* what) {
    if (result != FMOD_OK) {
        std::printf("FAIL %s: FMOD error %d\n", what, result);
        s_failures++;
        return false;
    }
    return true;
}

static void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        s_failures++;
    }
}

// --- Render notify: what CoreAudio actually pulled from the plugin ---------------------------------------------

static AudioStreamBasicDescription s_clientFormat;
static AudioStreamBasicDescription s_deviceFormat;
static std::atomic<uint64_t> s_frames;
static std::atomic<uint32_t> s_callbacks;
static std::atomic<uint32_t> s_peak;  // peak * 1e6

static float SampleAt(const AudioStreamBasicDescription& format, const void* data, uint32_t index) {
    if (format.mFormatFlags & kAudioFormatFlagIsFloat) {
        return format.mBitsPerChannel == 64 ? static_cast<float>(static_cast<const double*>(data)[index]) : static_cast<const float*>(data)[index];
    }
    switch (format.mBitsPerChannel) {
        case 16: return static_cast<const int16_t*>(data)[index] / 32768.0f;
        case 32: return static_cast<const int32_t*>(data)[index] / 2147483648.0f;
        case 24: {
            const uint8_t* p = static_cast<const uint8_t*>(data) + index * 3;
            int32_t v = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
            return v / 2147483648.0f;
        }
        default: return 0.0f;
    }
}

static OSStatus RenderNotify(void*, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* data) {
    if (!(*flags & kAudioUnitRenderAction_PostRender) || !data || !frames) {
        return noErr;
    }

    // The buffers are in the client (FMOD) format when one interleaved buffer matches it, else in the device format.
    const AudioStreamBasicDescription* format = &s_deviceFormat;
    if (data->mNumberBuffers == 1 && data->mBuffers[0].mNumberChannels == s_clientFormat.mChannelsPerFrame
        && data->mBuffers[0].mDataByteSize == frames * s_clientFormat.mBytesPerFrame) {
        format = &s_clientFormat;
    }

    float peak = 0.0f;
    for (UInt32 b = 0; b < data->mNumberBuffers; b++) {
        const AudioBuffer& buffer = data->mBuffers[b];
        uint32_t bytesPerSample = format->mBitsPerChannel / 8;
        if (!buffer.mData || !bytesPerSample) {
            continue;
        }
        uint32_t count = buffer.mDataByteSize / bytesPerSample;
        for (uint32_t i = 0; i < count; i++) {
            float v = std::fabs(SampleAt(*format, buffer.mData, i));
            if (v > peak && v <= 1.5f) {
                peak = v;
            }
        }
    }

    s_frames += frames;
    s_callbacks++;
    uint32_t scaled = static_cast<uint32_t>(peak * 1e6f);
    uint32_t current = s_peak.load();
    while (scaled > current && !s_peak.compare_exchange_weak(current, scaled)) {
    }
    return noErr;
}

static AudioUnit s_unit;

static bool AttachNotify(FMOD::System* system) {
    void* handle = nullptr;
    if (!Check(system->getOutputHandle(&handle), "getOutputHandle") || !handle) {
        Expect(false, "output handle is an AudioUnit");
        return false;
    }

    s_unit = static_cast<AudioUnit>(handle);
    UInt32 size = sizeof(s_clientFormat);
    AudioUnitGetProperty(s_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &s_clientFormat, &size);
    size = sizeof(s_deviceFormat);
    AudioUnitGetProperty(s_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &s_deviceFormat, &size);

    std::printf("  client format: %.0f Hz, %u ch, %u bit %s | device format: %.0f Hz, %u ch, %u bit %s%s\n",
        s_clientFormat.mSampleRate, s_clientFormat.mChannelsPerFrame, s_clientFormat.mBitsPerChannel,
        (s_clientFormat.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "int",
        s_deviceFormat.mSampleRate, s_deviceFormat.mChannelsPerFrame, s_deviceFormat.mBitsPerChannel,
        (s_deviceFormat.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "int",
        (s_deviceFormat.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? " non-interleaved" : "");

    return AudioUnitAddRenderNotify(s_unit, RenderNotify, nullptr) == noErr;
}

static void DetachNotify() {
    if (s_unit) {
        AudioUnitRemoveRenderNotify(s_unit, RenderNotify, nullptr);
        s_unit = nullptr;
    }
}

// Plays for `seconds` while calling System::update like the game's heartbeat, then checks what CoreAudio pulled.
static void PlayAndMeasure(FMOD::System* system, FMOD::Channel* channel, double seconds, const char* what) {
    s_frames = 0;
    s_callbacks = 0;
    s_peak = 0;

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        system->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (channel) {
        channel->stop();
    }
    system->update();

    double peak = s_peak.load() / 1e6;
    std::printf("  %-34s %8llu frames in %4u callbacks, peak %.3f\n", what, (unsigned long long)s_frames.load(), s_callbacks.load(), peak);

    char message[256];
    std::snprintf(message, sizeof(message), "%s: CoreAudio rendered frames", what);
    Expect(s_frames.load() > static_cast<uint64_t>(s_clientFormat.mSampleRate * seconds * 0.5), message);
    std::snprintf(message, sizeof(message), "%s: rendered audio is not silent", what);
    Expect(peak > 0.01, message);
}

// --- Sounds ----------------------------------------------------------------------------------------------------

static FMOD::Sound* CreateTone(FMOD::System* system, float frequency, float seconds) {
    const int rate = 44100;
    const int samples = static_cast<int>(rate * seconds);

    FMOD_CREATESOUNDEXINFO exinfo;
    std::memset(&exinfo, 0, sizeof(exinfo));
    exinfo.cbsize = sizeof(exinfo);
    exinfo.length = samples * sizeof(int16_t);
    exinfo.numchannels = 1;
    exinfo.defaultfrequency = rate;
    exinfo.format = FMOD_SOUND_FORMAT_PCM16;

    FMOD::Sound* sound = nullptr;
    if (!Check(system->createSound(nullptr, FMOD_SOFTWARE | FMOD_OPENUSER | FMOD_LOOP_NORMAL | FMOD_2D, &exinfo, &sound), "createSound tone")) {
        return nullptr;
    }

    void *ptr1, *ptr2;
    unsigned int len1, len2;
    if (Check(sound->lock(0, exinfo.length, &ptr1, &ptr2, &len1, &len2), "Sound::lock")) {
        int16_t* pcm = static_cast<int16_t*>(ptr1);
        for (unsigned int i = 0; i < len1 / sizeof(int16_t); i++) {
            pcm[i] = static_cast<int16_t>(0.5f * 32767.0f * std::sin(2.0 * M_PI * frequency * i / rate));
        }
        sound->unlock(ptr1, ptr2, len1, len2);
    }
    return sound;
}

static void PlayTone(FMOD::System* system, float frequency, float seconds, float pan, const char* what) {
    FMOD::Sound* tone = CreateTone(system, frequency, 1.0f);
    if (!tone) {
        return;
    }

    FMOD::Channel* channel = nullptr;
    if (Check(system->playSound(FMOD_CHANNEL_FREE, tone, true, &channel), "playSound tone")) {
        channel->setVolume(0.2f);
        channel->setPan(pan);
        channel->setPaused(false);
        PlayAndMeasure(system, channel, seconds, what);
    }
    tone->release();
}

static std::vector<std::string> s_soundNames;

static int32_t CollectSoundName(const char* fileName, void* param) {
    std::string name = fileName;
    if (name.size() > 4 && !SStrCmpI(name.c_str(), "Sound\\", 6)) {
        s_soundNames.push_back(name);
    }
    return 1;
}

static bool FindGameSound(const char* prefix, const char* extension, std::string& out) {
    size_t prefixLength = std::strlen(prefix);
    for (const std::string& name : s_soundNames) {
        if (name.size() > prefixLength + 4 && !SStrCmpI(name.c_str(), prefix, prefixLength)
            && !SStrCmpI(name.c_str() + name.size() - 4, extension, STORM_MAX_STR)) {
            out = name;
            return true;
        }
    }
    return false;
}

static void PlayGameFile(FMOD::System* system, const char* prefix, const char* extension, double seconds) {
    std::string name;
    if (!FindGameSound(prefix, extension, name)) {
        std::printf("FAIL no %s*%s file in the archives\n", prefix, extension);
        s_failures++;
        return;
    }

    void* data = nullptr;
    uint32_t size = 0;
    if (!SFile::LoadFile(name.c_str(), &data, &size, 0, nullptr)) {
        std::printf("FAIL SFile::LoadFile %s\n", name.c_str());
        s_failures++;
        return;
    }

    FMOD_CREATESOUNDEXINFO exinfo;
    std::memset(&exinfo, 0, sizeof(exinfo));
    exinfo.cbsize = sizeof(exinfo);
    exinfo.length = size;

    FMOD::Sound* sound = nullptr;
    std::string what = "game file " + name;
    if (Check(system->createSound(static_cast<const char*>(data), FMOD_SOFTWARE | FMOD_OPENMEMORY | FMOD_2D | FMOD_LOOP_NORMAL, &exinfo, &sound), what.c_str())) {
        FMOD::Channel* channel = nullptr;
        if (Check(system->playSound(FMOD_CHANNEL_FREE, sound, true, &channel), "playSound game file")) {
            channel->setVolume(0.2f);
            channel->setPaused(false);
            if (what.size() > 34) {
                what = "..." + what.substr(what.size() - 31);
            }
            PlayAndMeasure(system, channel, seconds, what.c_str());
        }
        sound->release();
    }
    SFile::Unload(data);
}

// --- FMOD setup ------------------------------------------------------------------------------------------------

static const char* const s_speakerModes[] = { "RAW", "MONO", "STEREO", "QUAD", "SURROUND", "5.1", "7.1", "PROLOGIC" };

// Same sequence as SoundEngine.cpp's software path; speakerMode/format < 0 keep what the game would pick.
static FMOD::System* CreateSystem(int speakerMode, FMOD_SOUND_FORMAT format, int rate, bool verbose) {
    FMOD::System* system = nullptr;
    if (!Check(FMOD::System_Create(&system), "System_Create")) {
        return nullptr;
    }

    Check(system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT), "setOutput AUTODETECT");

    int numDrivers = 0;
    Check(system->getNumDrivers(&numDrivers), "getNumDrivers");
    Expect(numDrivers > 0, "at least one output driver");
    if (verbose) {
        std::printf("  %d output drivers\n", numDrivers);
        for (int i = 0; i < numDrivers; i++) {
            char name[256] = {};
            system->getDriverName(i, name, sizeof(name));
            FMOD_CAPS caps = 0;
            FMOD_SPEAKERMODE mode = FMOD_SPEAKERMODE_STEREO;
            system->getDriverCaps(i, &caps, nullptr, nullptr, &mode);
            std::printf("  --- [%d] %s (caps 0x%x, speaker mode %s)\n", i, name, caps, s_speakerModes[mode]);
        }
    }

    Check(system->setDriver(0), "setDriver");
    Check(system->setHardwareChannels(0, 0, 0, 0), "setHardwareChannels");
    Check(system->setSoftwareChannels(32), "setSoftwareChannels");

    FMOD_CAPS caps = 0;
    FMOD_SPEAKERMODE controlPanelMode = FMOD_SPEAKERMODE_STEREO;
    Check(system->getDriverCaps(0, &caps, nullptr, nullptr, &controlPanelMode), "getDriverCaps");
    Check(system->setDSPBufferSize(1024, 4), "setDSPBufferSize");

    FMOD_SPEAKERMODE mode = speakerMode < 0 ? controlPanelMode : static_cast<FMOD_SPEAKERMODE>(speakerMode);
    Check(system->setSpeakerMode(mode), "setSpeakerMode");
    if (format != FMOD_SOUND_FORMAT_NONE) {
        Check(system->setSoftwareFormat(rate, format, 0, 0, FMOD_DSP_RESAMPLER_LINEAR), "setSoftwareFormat");
    }

    if (!Check(system->init(64, FMOD_INIT_3D_RIGHTHANDED, nullptr), "System::init")) {
        system->release();
        return nullptr;
    }

    FMOD_OUTPUTTYPE output;
    system->getOutput(&output);
    Expect(output == FMOD_OUTPUTTYPE_COREAUDIO, "AUTODETECT picked FMOD_OUTPUTTYPE_COREAUDIO");

    int outRate = 0, outChannels = 0, bits = 0;
    FMOD_SOUND_FORMAT outFormat;
    system->getSoftwareFormat(&outRate, &outFormat, &outChannels, nullptr, nullptr, &bits);
    std::printf("  init: output %d, speaker mode %s (control panel %s), %d Hz, %d ch, %d bit%s\n", output, s_speakerModes[mode],
        s_speakerModes[controlPanelMode], outRate, outChannels, bits, outFormat == FMOD_SOUND_FORMAT_PCMFLOAT ? " float" : "");

    if (!AttachNotify(system)) {
        Expect(false, "AudioUnitAddRenderNotify");
    }
    return system;
}

static void DestroySystem(FMOD::System* system) {
    DetachNotify();
    Check(system->close(), "System::close");
    Check(system->release(), "System::release");
}

int main(int argc, char* argv[]) {
    // 1. The game's configuration, with game files when the data folder was given.
    std::printf("[game configuration]\n");
    if (FMOD::System* system = CreateSystem(-1, FMOD_SOUND_FORMAT_NONE, 0, true)) {
        PlayTone(system, 440.0f, 1.5f, 0.0f, "tone 440 Hz");

        if (argc > 1) {
            StormRtlInitialize();
            StormInitialize();
            SFile::DisableSFileCheckDisk();
            SFile::EnableDirectAccess(0);
            SFile::SetBasePath(argv[1]);
            SFile::SetDataPath("Data\\");

            const char* archives[] = { "common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ", "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ" };
            int32_t priority = 100;
            for (const char* name : archives) {
                char path[260];
                SStrPrintf(path, sizeof(path), "Data\\%s", name);
                SArchive* archive = nullptr;
                if (SFile::OpenArchive(path, priority--, 0xC00, &archive) && archive) {
                    SFile::List(archive, CollectSoundName, nullptr);
                }
            }
            std::printf("  %zu Sound\\ files in the archives\n", s_soundNames.size());

            PlayGameFile(system, "Sound\\Interface\\", ".wav", 1.5);
            PlayGameFile(system, "Sound\\Music\\", ".mp3", 3.0);
        } else {
            std::printf("  (no game folder given: skipping the game files)\n");
        }

        DestroySystem(system);
    }

    // 2. Mix Mode 2 style float output at 44.1 kHz.
    std::printf("[PCMFLOAT 44100 stereo]\n");
    if (FMOD::System* system = CreateSystem(FMOD_SPEAKERMODE_STEREO, FMOD_SOUND_FORMAT_PCMFLOAT, 44100, false)) {
        PlayTone(system, 660.0f, 0.8f, -1.0f, "tone 660 Hz float");
        DestroySystem(system);
    }

    // 3. Multichannel speaker modes, tone panned to front left so it is audible on a stereo device too.
    std::printf("[5.1]\n");
    if (FMOD::System* system = CreateSystem(FMOD_SPEAKERMODE_5POINT1, FMOD_SOUND_FORMAT_PCM16, 48000, false)) {
        PlayTone(system, 550.0f, 0.8f, -1.0f, "tone 550 Hz 5.1");
        DestroySystem(system);
    }

    std::printf("[7.1]\n");
    if (FMOD::System* system = CreateSystem(FMOD_SPEAKERMODE_7POINT1, FMOD_SOUND_FORMAT_PCM24, 48000, false)) {
        PlayTone(system, 495.0f, 0.8f, -1.0f, "tone 495 Hz 7.1 PCM24");
        DestroySystem(system);
    }

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}
