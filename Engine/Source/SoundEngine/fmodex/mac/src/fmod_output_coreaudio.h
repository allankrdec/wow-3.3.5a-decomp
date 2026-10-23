/*
    FMOD Ex CoreAudio output for macOS (wowdecomp port).

    A polled software-mixer output, like the WinMM and DirectSound outputs: FMOD's mixer thread fills a ring buffer of
    dspnumbuffers blocks through getPosition / lock, and an AudioUnit render callback plays that ring buffer and moves
    the play cursor.  Driver 0 is the system default output (kAudioUnitSubType_DefaultOutput, which follows changes
    of the default device), drivers 1..n are the individual CoreAudio output devices (kAudioUnitSubType_HALOutput).
*/

#ifndef _FMOD_OUTPUT_COREAUDIO_H
#define _FMOD_OUTPUT_COREAUDIO_H

#include "fmod_settings.h"

#ifdef FMOD_SUPPORT_COREAUDIO

#include "fmod_output_polled.h"

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

namespace FMOD
{
    const int COREAUDIO_MAXDRIVERS        = 64;
    const int COREAUDIO_DRIVERNAMELENGTH  = 256;

    class OutputCoreAudio : public OutputPolled
    {
      private:

        /*
            Driver list.  Entry 0 is the system default output, entries 1..n are the output devices.
        */
        int                 mNumDrivers;
        AudioDeviceID       mDriverDevice[COREAUDIO_MAXDRIVERS];
        char                mDriverName[COREAUDIO_MAXDRIVERS][COREAUDIO_DRIVERNAMELENGTH];

        /*
            Playback information.  Members are written by the render callback (CoreAudio IO thread) and read by the
            FMOD mixer thread, so the cursor is only ever accessed atomically.
        */
        AudioUnit           mAudioUnit;
        bool                mUnitInitialized;
        bool                mRunning;
        char               *mBuffer;
        unsigned int        mBufferLengthBytes;
        unsigned int        mBufferLengthSamples;
        unsigned int        mBlockAlign;
        unsigned int        mPlayPositionSamples;       /* atomic, always < mBufferLengthSamples */

        FMOD_RESULT          enumerate();
        FMOD_RESULT          getSpeakerModeForDevice(AudioDeviceID device, FMOD_SPEAKERMODE *speakermode);
        FMOD_RESULT          setChannelMap(AudioDeviceID device, int outputchannels, FMOD_SPEAKERMODE speakermode);

        static OSStatus      renderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);

      public:

        static FMOD_OUTPUT_DESCRIPTION_EX *getDescriptionEx();

        FMOD_RESULT              getNumDrivers(int *numdrivers);
        FMOD_RESULT              getDriverName(int id, char *name, int namelen);
        FMOD_RESULT              getDriverCapsEx(int id, FMOD_CAPS *caps, int *minfrequency, int *maxfrequency, FMOD_SPEAKERMODE *controlpanelspeakermode);
        FMOD_RESULT              initEx(int selecteddriver, FMOD_INITFLAGS flags, int *outputrate, int outputchannels, FMOD_SOUND_FORMAT *outputformat, FMOD_SPEAKERMODE *speakermode, int dspbufferlength, int dspnumbuffers, void *extradriverdata);
        FMOD_RESULT              close();
        FMOD_RESULT              getHandle(void **handle);
        FMOD_RESULT              start();
        FMOD_RESULT              stop();
        FMOD_RESULT              getPosition(unsigned int *pcm);
        FMOD_RESULT              lock(unsigned int offset, unsigned int length, void **ptr1, void **ptr2, unsigned int *len1, unsigned int *len2);

        static FMOD_RESULT F_CALLBACK getNumDriversCallback         (FMOD_OUTPUT_STATE *output, int *numdrivers);
        static FMOD_RESULT F_CALLBACK getDriverNameCallback         (FMOD_OUTPUT_STATE *output, int id, char *name, int namelen);
        static FMOD_RESULT F_CALLBACK getDriverCapsExCallback       (FMOD_OUTPUT_STATE *output, int id, FMOD_CAPS *caps, int *minfrequency, int *maxfrequency, FMOD_SPEAKERMODE *controlpanelspeakermode);
        static FMOD_RESULT F_CALLBACK initExCallback                (FMOD_OUTPUT_STATE *output, int selecteddriver, FMOD_INITFLAGS flags, int *outputrate, int outputchannels, FMOD_SOUND_FORMAT *outputformat, FMOD_SPEAKERMODE *speakermode, int dspbufferlength, int dspnumbuffers, int max2dchannels, int max3dchannels, void *extradriverdata);
        static FMOD_RESULT F_CALLBACK closeCallback                 (FMOD_OUTPUT_STATE *output);
        static FMOD_RESULT F_CALLBACK startCallback                 (FMOD_OUTPUT_STATE *output);
        static FMOD_RESULT F_CALLBACK stopCallback                  (FMOD_OUTPUT_STATE *output);
        static FMOD_RESULT F_CALLBACK getHandleCallback             (FMOD_OUTPUT_STATE *output, void **handle);
        static FMOD_RESULT F_CALLBACK getPositionCallback           (FMOD_OUTPUT_STATE *output, unsigned int *pcm);
        static FMOD_RESULT F_CALLBACK lockCallback                  (FMOD_OUTPUT_STATE *output, unsigned int offset, unsigned int length, void **ptr1, void **ptr2, unsigned int *len1, unsigned int *len2);
    };
}

/*
    Called by FMOD_OS_CheckDriverList: true once after the CoreAudio device list changed since the previous call.
*/
FMOD_RESULT FMOD_OS_CoreAudio_CheckDriverList(bool *devicelistchanged);

#endif  /* #ifdef FMOD_SUPPORT_COREAUDIO */

#endif
