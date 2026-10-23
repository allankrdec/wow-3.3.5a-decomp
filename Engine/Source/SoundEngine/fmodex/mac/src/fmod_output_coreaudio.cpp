/*
    FMOD Ex CoreAudio output for macOS (wowdecomp port).  See fmod_output_coreaudio.h.
*/

#include "fmod_settings.h"

#ifdef FMOD_SUPPORT_COREAUDIO

#include "fmod.h"
#include "fmod_debug.h"
#include "fmod_memory.h"
#include "fmod_output_coreaudio.h"
#include "fmod_soundi.h"
#include "fmod_string.h"
#include "fmod_systemi.h"

#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>

namespace FMOD
{

FMOD_OUTPUT_DESCRIPTION_EX coreaudiooutput;


/*
    Small CoreAudio helpers.
*/
static FMOD_RESULT CoreAudio_GetDefaultOutputDevice(AudioDeviceID *device)
{
    AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32                     size    = sizeof(AudioDeviceID);

    *device = kAudioObjectUnknown;

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, 0, &size, device) != noErr || *device == kAudioObjectUnknown)
    {
        return FMOD_ERR_OUTPUT_INIT;
    }

    return FMOD_OK;
}


static int CoreAudio_GetNumOutputChannels(AudioDeviceID device)
{
    AudioObjectPropertyAddress address = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32                     size    = 0;
    AudioBufferList           *list;
    int                        channels = 0;

    if (AudioObjectGetPropertyDataSize(device, &address, 0, 0, &size) != noErr || !size)
    {
        return 0;
    }

    list = (AudioBufferList *)FMOD_Memory_Calloc(size);
    if (!list)
    {
        return 0;
    }

    if (AudioObjectGetPropertyData(device, &address, 0, 0, &size, list) == noErr)
    {
        for (UInt32 count = 0; count < list->mNumberBuffers; count++)
        {
            channels += list->mBuffers[count].mNumberChannels;
        }
    }

    FMOD_Memory_Free(list);

    return channels;
}


static void CoreAudio_GetDeviceName(AudioDeviceID device, char *name, int namelen)
{
    AudioObjectPropertyAddress address = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef                string  = 0;
    UInt32                     size    = sizeof(CFStringRef);

    name[0] = 0;

    if (AudioObjectGetPropertyData(device, &address, 0, 0, &size, &string) == noErr && string)
    {
        if (!CFStringGetCString(string, name, namelen, kCFStringEncodingUTF8))
        {
            name[0] = 0;
        }
        CFRelease(string);
    }

    if (!name[0])
    {
        FMOD_strncpy(name, "Unknown CoreAudio Device", namelen);
    }
}


/*
    Reads the speaker labels of the device's preferred channel layout (Audio MIDI Setup -> Configure Speakers).
    labels[] receives one label per device output channel.  Returns the number of channels filled in.
*/
static int CoreAudio_GetDeviceChannelLabels(AudioDeviceID device, AudioChannelLabel *labels, int maxlabels)
{
    AudioObjectPropertyAddress address = { kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32                     size    = 0;
    AudioChannelLayout        *layout;
    AudioChannelLayout        *expanded = 0;
    int                        numlabels = 0;

    if (AudioObjectGetPropertyDataSize(device, &address, 0, 0, &size) != noErr || !size)
    {
        return 0;
    }

    layout = (AudioChannelLayout *)FMOD_Memory_Calloc(size);
    if (!layout)
    {
        return 0;
    }

    if (AudioObjectGetPropertyData(device, &address, 0, 0, &size, layout) != noErr)
    {
        FMOD_Memory_Free(layout);
        return 0;
    }

    if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions)
    {
        /*
            Expand a tag or bitmap layout into channel descriptions.
        */
        AudioFormatPropertyID  property = kAudioFormatProperty_ChannelLayoutForTag;
        const void            *specifier = &layout->mChannelLayoutTag;
        UInt32                 specifiersize = sizeof(AudioChannelLayoutTag);
        UInt32                 expandedsize = 0;

        if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap)
        {
            property      = kAudioFormatProperty_ChannelLayoutForBitmap;
            specifier     = &layout->mChannelBitmap;
            specifiersize = sizeof(AudioChannelBitmap);
        }

        if (AudioFormatGetPropertyInfo(property, specifiersize, specifier, &expandedsize) == noErr && expandedsize)
        {
            expanded = (AudioChannelLayout *)FMOD_Memory_Calloc(expandedsize);
            if (expanded && AudioFormatGetProperty(property, specifiersize, specifier, &expandedsize, expanded) != noErr)
            {
                FMOD_Memory_Free(expanded);
                expanded = 0;
            }
        }
    }

    {
        AudioChannelLayout *source = expanded ? expanded : layout;

        if (source->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions)
        {
            for (UInt32 count = 0; count < source->mNumberChannelDescriptions && numlabels < maxlabels; count++)
            {
                labels[numlabels++] = source->mChannelDescriptions[count].mChannelLabel;
            }
        }
    }

    if (expanded)
    {
        FMOD_Memory_Free(expanded);
    }
    FMOD_Memory_Free(layout);

    return numlabels;
}


static int CoreAudio_FindLabel(const AudioChannelLabel *labels, int numlabels, AudioChannelLabel label)
{
    for (int count = 0; count < numlabels; count++)
    {
        if (labels[count] == label)
        {
            return count;
        }
    }

    return -1;
}


#ifdef PLUGIN_EXPORTS

#ifdef __cplusplus
extern "C" {
#endif

    /*
        FMODGetOutputDescription is mandantory for every fmod plugin.  This is the symbol the registerplugin function searches for.
    */
    F_DECLSPEC F_DLLEXPORT FMOD_OUTPUT_DESCRIPTION_EX * F_API FMODGetOutputDescriptionEx()
    {
        return OutputCoreAudio::getDescriptionEx();
    }

#ifdef __cplusplus
}
#endif

#endif  /* PLUGIN_EXPORTS */


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_OUTPUT_DESCRIPTION_EX *OutputCoreAudio::getDescriptionEx()
{
    FMOD_memset(&coreaudiooutput, 0, sizeof(FMOD_OUTPUT_DESCRIPTION_EX));

    coreaudiooutput.name                = "FMOD CoreAudio Output";
    coreaudiooutput.version             = 0x00010100;
    coreaudiooutput.polling             = true;
    coreaudiooutput.getnumdrivers       = &OutputCoreAudio::getNumDriversCallback;
    coreaudiooutput.getdrivername       = &OutputCoreAudio::getDriverNameCallback;
    coreaudiooutput.getdrivercapsex     = &OutputCoreAudio::getDriverCapsExCallback;
    coreaudiooutput.initex              = &OutputCoreAudio::initExCallback;
    coreaudiooutput.close               = &OutputCoreAudio::closeCallback;
    coreaudiooutput.start               = &OutputCoreAudio::startCallback;
    coreaudiooutput.stop                = &OutputCoreAudio::stopCallback;
    coreaudiooutput.gethandle           = &OutputCoreAudio::getHandleCallback;
    coreaudiooutput.getposition         = &OutputCoreAudio::getPositionCallback;
    coreaudiooutput.lock                = &OutputCoreAudio::lockCallback;

    coreaudiooutput.mType               = FMOD_OUTPUTTYPE_COREAUDIO;
    coreaudiooutput.mSize               = sizeof(OutputCoreAudio);

    return &coreaudiooutput;
}


/*
[
	[DESCRIPTION]
    Builds the driver list: the system default output first, then every device that has output channels.

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::enumerate()
{
    AudioObjectPropertyAddress  address = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioDeviceID               defaultdevice;
    AudioDeviceID              *devices;
    UInt32                      size = 0;
    int                         numdevices;

    mNumDrivers = 0;

    if (CoreAudio_GetDefaultOutputDevice(&defaultdevice) != FMOD_OK)
    {
        mEnumerated = true;
        return FMOD_OK;
    }

    /*
        Driver 0 always follows the system default output device.
    */
    mDriverDevice[0] = kAudioObjectUnknown;
    FMOD_strncpy(mDriverName[0], "System Default Output", COREAUDIO_DRIVERNAMELENGTH);
    mNumDrivers = 1;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, 0, &size) != noErr || !size)
    {
        mEnumerated = true;
        return FMOD_OK;
    }

    devices = (AudioDeviceID *)FMOD_Memory_Calloc(size);
    if (!devices)
    {
        return FMOD_ERR_MEMORY;
    }

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, 0, &size, devices) == noErr)
    {
        numdevices = (int)(size / sizeof(AudioDeviceID));

        for (int count = 0; count < numdevices && mNumDrivers < COREAUDIO_MAXDRIVERS; count++)
        {
            if (CoreAudio_GetNumOutputChannels(devices[count]) <= 0)
            {
                continue;
            }

            mDriverDevice[mNumDrivers] = devices[count];
            CoreAudio_GetDeviceName(devices[count], mDriverName[mNumDrivers], COREAUDIO_DRIVERNAMELENGTH);
            mNumDrivers++;
        }
    }

    FMOD_Memory_Free(devices);

    mEnumerated = true;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::getNumDrivers(int *numdrivers)
{
    FMOD_RESULT result;

    result = enumerate();
    if (result != FMOD_OK)
    {
        return result;
    }

    *numdrivers = mNumDrivers;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::getDriverName(int id, char *name, int namelen)
{
    FMOD_RESULT result;

    if (!mEnumerated)
    {
        result = enumerate();
        if (result != FMOD_OK)
        {
            return result;
        }
    }

    if (id < 0 || id >= mNumDrivers)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    FMOD_strncpy(name, mDriverName[id], namelen);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]
    Derives the speaker mode from the speakers configured for the device in Audio MIDI Setup.

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::getSpeakerModeForDevice(AudioDeviceID device, FMOD_SPEAKERMODE *speakermode)
{
    AudioChannelLabel   labels[64];
    int                 numlabels;
    int                 numchannels;
    bool                center, lfe;
    int                 pairs = 0;

    *speakermode = FMOD_SPEAKERMODE_STEREO;

    numchannels = CoreAudio_GetNumOutputChannels(device);
    if (numchannels == 1)
    {
        *speakermode = FMOD_SPEAKERMODE_MONO;
        return FMOD_OK;
    }
    if (numchannels < 4)
    {
        return FMOD_OK;
    }

    numlabels = CoreAudio_GetDeviceChannelLabels(device, labels, 64);

    center = CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_Center) >= 0;
    lfe    = CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_LFEScreen) >= 0 || CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_LFE2) >= 0;

    if (CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_LeftSurround) >= 0 && CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_RightSurround) >= 0)
    {
        pairs++;
    }
    if (CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_RearSurroundLeft) >= 0 && CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_RearSurroundRight) >= 0)
    {
        pairs++;
    }
    if (CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_LeftSurroundDirect) >= 0 && CoreAudio_FindLabel(labels, numlabels, kAudioChannelLabel_RightSurroundDirect) >= 0)
    {
        pairs++;
    }

    if (center && lfe && pairs >= 2 && numchannels >= 8)
    {
        *speakermode = FMOD_SPEAKERMODE_7POINT1;
    }
    else if (center && lfe && pairs >= 1 && numchannels >= 6)
    {
        *speakermode = FMOD_SPEAKERMODE_5POINT1;
    }
    else if (center && pairs >= 1 && numchannels >= 5)
    {
        *speakermode = FMOD_SPEAKERMODE_SURROUND;
    }
    else if (pairs >= 1)
    {
        *speakermode = FMOD_SPEAKERMODE_QUAD;
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]
    Every Mac output device takes 16/24/32 bit integer and float PCM through the AudioUnit's format converter, and
    the converter also resamples, so any rate works.

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::getDriverCapsEx(int id, FMOD_CAPS *caps, int *minfrequency, int *maxfrequency, FMOD_SPEAKERMODE *controlpanelspeakermode)
{
    FMOD_RESULT     result;
    AudioDeviceID   device;

    if (!mEnumerated)
    {
        result = enumerate();
        if (result != FMOD_OK)
        {
            return result;
        }
    }

    if (id < 0 || id >= mNumDrivers)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    device = mDriverDevice[id];
    if (id == 0)
    {
        result = CoreAudio_GetDefaultOutputDevice(&device);
        if (result != FMOD_OK)
        {
            return result;
        }
    }

    *caps = (FMOD_CAPS)(*caps | FMOD_CAPS_OUTPUT_FORMAT_PCM16 | FMOD_CAPS_OUTPUT_FORMAT_PCM24 | FMOD_CAPS_OUTPUT_FORMAT_PCM32 | FMOD_CAPS_OUTPUT_FORMAT_PCMFLOAT);
    if (CoreAudio_GetNumOutputChannels(device) > 2)
    {
        *caps = (FMOD_CAPS)(*caps | FMOD_CAPS_OUTPUT_MULTICHANNEL);
    }

    *minfrequency = 8000;
    *maxfrequency = 192000;

    return getSpeakerModeForDevice(device, controlpanelspeakermode);
}


/*
[
	[DESCRIPTION]
    Routes FMOD's interleaved channels to the device channels that carry the same speakers, using the device's
    preferred stereo pair and preferred channel layout.  Without a usable layout the channels go out in order.

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::setChannelMap(AudioDeviceID device, int outputchannels, FMOD_SPEAKERMODE speakermode)
{
    SInt32              map[64];
    int                 numdevicechannels;
    AudioChannelLabel   labels[64];
    int                 numlabels;

    numdevicechannels = CoreAudio_GetNumOutputChannels(device);
    if (numdevicechannels <= 0 || numdevicechannels > 64)
    {
        return FMOD_OK;
    }

    for (int count = 0; count < numdevicechannels; count++)
    {
        map[count] = -1;
    }

    if (outputchannels <= 2)
    {
        AudioObjectPropertyAddress  address = { kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        UInt32                      stereo[2] = { 1, 2 };
        UInt32                      size = sizeof(stereo);

        AudioObjectGetPropertyData(device, &address, 0, 0, &size, stereo);

        if (stereo[0] < 1 || (int)stereo[0] > numdevicechannels || stereo[1] < 1 || (int)stereo[1] > numdevicechannels)
        {
            return FMOD_OK;
        }

        if (numdevicechannels == 1)
        {
            return FMOD_OK;
        }

        map[stereo[0] - 1] = 0;
        map[stereo[1] - 1] = (outputchannels == 2) ? 1 : 0;   /* Mono goes to both speakers of the stereo pair. */
    }
    else
    {
        /*
            Candidate device speakers for each FMOD output channel, in FMOD's channel order.
        */
        static const AudioChannelLabel left[]   = { kAudioChannelLabel_Left, 0 };
        static const AudioChannelLabel right[]  = { kAudioChannelLabel_Right, 0 };
        static const AudioChannelLabel center[] = { kAudioChannelLabel_Center, 0 };
        static const AudioChannelLabel lfe[]    = { kAudioChannelLabel_LFEScreen, kAudioChannelLabel_LFE2, 0 };
        static const AudioChannelLabel bl[]     = { kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RearSurroundLeft, kAudioChannelLabel_LeftSurroundDirect, 0 };
        static const AudioChannelLabel br[]     = { kAudioChannelLabel_RightSurround, kAudioChannelLabel_RearSurroundRight, kAudioChannelLabel_RightSurroundDirect, 0 };
        static const AudioChannelLabel bl71[]   = { kAudioChannelLabel_RearSurroundLeft, kAudioChannelLabel_LeftSurround, 0 };
        static const AudioChannelLabel br71[]   = { kAudioChannelLabel_RearSurroundRight, kAudioChannelLabel_RightSurround, 0 };
        static const AudioChannelLabel sl71[]   = { kAudioChannelLabel_LeftSurroundDirect, kAudioChannelLabel_LeftSurround, 0 };
        static const AudioChannelLabel sr71[]   = { kAudioChannelLabel_RightSurroundDirect, kAudioChannelLabel_RightSurround, 0 };

        const AudioChannelLabel *speakers[8];
        int                      numspeakers = 0;
        bool                     used[64];

        switch (speakermode)
        {
            case FMOD_SPEAKERMODE_QUAD:
            {
                speakers[0] = left; speakers[1] = right; speakers[2] = bl; speakers[3] = br;
                numspeakers = 4;
                break;
            }
            case FMOD_SPEAKERMODE_SURROUND:
            {
                speakers[0] = left; speakers[1] = right; speakers[2] = center; speakers[3] = bl; speakers[4] = br;
                numspeakers = 5;
                break;
            }
            case FMOD_SPEAKERMODE_5POINT1:
            {
                speakers[0] = left; speakers[1] = right; speakers[2] = center; speakers[3] = lfe; speakers[4] = bl; speakers[5] = br;
                numspeakers = 6;
                break;
            }
            case FMOD_SPEAKERMODE_7POINT1:
            {
                speakers[0] = left; speakers[1] = right; speakers[2] = center; speakers[3] = lfe; speakers[4] = bl71; speakers[5] = br71; speakers[6] = sl71; speakers[7] = sr71;
                numspeakers = 8;
                break;
            }
            default:
            {
                return FMOD_OK;     /* RAW: channels in order. */
            }
        }

        if (numspeakers != outputchannels)
        {
            return FMOD_OK;
        }

        numlabels = CoreAudio_GetDeviceChannelLabels(device, labels, 64);
        if (numlabels < numdevicechannels)
        {
            return FMOD_OK;
        }

        FMOD_memset(used, 0, sizeof(used));

        for (int channel = 0; channel < numspeakers; channel++)
        {
            int found = -1;

            for (const AudioChannelLabel *label = speakers[channel]; *label && found < 0; label++)
            {
                for (int count = 0; count < numdevicechannels; count++)
                {
                    if (!used[count] && labels[count] == *label)
                    {
                        found = count;
                        break;
                    }
                }
            }

            if (found < 0)
            {
                FLOG((FMOD_DEBUG_LEVEL_LOG, __FILE__, __LINE__, "OutputCoreAudio::setChannelMap", "Device has no speaker for output channel %d, sending channels in order.\n", channel));
                return FMOD_OK;
            }

            used[found] = true;
            map[found]  = channel;
        }
    }

    /*
        Channels in order is the AudioUnit's default.  Leaving the map unset then also keeps the default output unit
        correct when it follows the system default to a device with a different channel count.
    */
    {
        bool identity = true;

        for (int count = 0; count < numdevicechannels; count++)
        {
            if (map[count] != (count < outputchannels ? count : -1))
            {
                identity = false;
                break;
            }
        }

        if (identity)
        {
            return FMOD_OK;
        }
    }

    if (AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 0, map, numdevicechannels * sizeof(SInt32)) != noErr)
    {
        FLOG((FMOD_DEBUG_LEVEL_LOG, __FILE__, __LINE__, "OutputCoreAudio::setChannelMap", "kAudioOutputUnitProperty_ChannelMap failed, sending channels in order.\n"));
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::initEx(int selecteddriver, FMOD_INITFLAGS flags, int *outputrate, int outputchannels, FMOD_SOUND_FORMAT *outputformat, FMOD_SPEAKERMODE *speakermode, int dspbufferlength, int dspnumbuffers, void *extradriverdata)
{
    FMOD_RESULT                 result;
    AudioComponentDescription   componentdesc;
    AudioComponent              component;
    AudioStreamBasicDescription format;
    AURenderCallbackStruct      callback;
    AudioDeviceID               device;
    UInt32                      size;
    int                         bits;

    Plugin::init(); /* Set up gGlobal - for debug / file / memory access by this plugin. */

    FLOG((FMOD_DEBUG_LEVEL_ALL, __FILE__, __LINE__, "OutputCoreAudio::initEx", "Initializing.\n"));

    result = enumerate();
    if (result != FMOD_OK)
    {
        return result;
    }

    if (!mNumDrivers)
    {
        return FMOD_ERR_OUTPUT_INIT;
    }

    if (selecteddriver < 0 || selecteddriver >= mNumDrivers)
    {
        selecteddriver = 0;
    }

    if (outputchannels < 1 || outputchannels > 8)
    {
        return FMOD_ERR_OUTPUT_FORMAT;
    }

    /*
        The AudioUnit converter takes signed integer and float PCM.  Unsigned 8 bit is mixed as 16 bit instead.
    */
    if (*outputformat != FMOD_SOUND_FORMAT_PCM16 && *outputformat != FMOD_SOUND_FORMAT_PCM24 &&
        *outputformat != FMOD_SOUND_FORMAT_PCM32 && *outputformat != FMOD_SOUND_FORMAT_PCMFLOAT)
    {
        *outputformat = FMOD_SOUND_FORMAT_PCM16;
    }

    result = SoundI::getBitsFromFormat(*outputformat, &bits);
    if (result != FMOD_OK)
    {
        return result;
    }

    /*
        Open the AudioUnit: DefaultOutput follows the system default device, HALOutput is bound to one device.
    */
    FMOD_memset(&componentdesc, 0, sizeof(AudioComponentDescription));
    componentdesc.componentType         = kAudioUnitType_Output;
    componentdesc.componentSubType      = selecteddriver == 0 ? kAudioUnitSubType_DefaultOutput : kAudioUnitSubType_HALOutput;
    componentdesc.componentManufacturer = kAudioUnitManufacturer_Apple;

    component = AudioComponentFindNext(0, &componentdesc);
    if (!component)
    {
        return FMOD_ERR_OUTPUT_INIT;
    }

    if (AudioComponentInstanceNew(component, &mAudioUnit) != noErr)
    {
        mAudioUnit = 0;
        return FMOD_ERR_OUTPUT_INIT;
    }

    if (selecteddriver != 0)
    {
        device = mDriverDevice[selecteddriver];

        if (AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(AudioDeviceID)) != noErr)
        {
            FLOG((FMOD_DEBUG_LEVEL_ERROR, __FILE__, __LINE__, "OutputCoreAudio::initEx", "Cannot select device %d (%s).\n", device, mDriverName[selecteddriver]));
            close();
            return FMOD_ERR_OUTPUT_INIT;
        }
    }
    else
    {
        size = sizeof(AudioDeviceID);
        if (AudioUnitGetProperty(mAudioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, &size) != noErr)
        {
            CoreAudio_GetDefaultOutputDevice(&device);
        }
    }

    /*
        The format FMOD mixes in: interleaved, packed, native endian.
    */
    FMOD_memset(&format, 0, sizeof(AudioStreamBasicDescription));
    format.mSampleRate       = (Float64)*outputrate;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    format.mFormatFlags     |= (*outputformat == FMOD_SOUND_FORMAT_PCMFLOAT) ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger;
    format.mChannelsPerFrame = outputchannels;
    format.mBitsPerChannel   = bits;
    format.mBytesPerFrame    = outputchannels * bits / 8;
    format.mFramesPerPacket  = 1;
    format.mBytesPerPacket   = format.mBytesPerFrame;

    if (AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(AudioStreamBasicDescription)) != noErr)
    {
        FLOG((FMOD_DEBUG_LEVEL_ERROR, __FILE__, __LINE__, "OutputCoreAudio::initEx", "Device does not take %d channels, %d bits, %d hz.\n", outputchannels, bits, *outputrate));
        close();
        return FMOD_ERR_OUTPUT_CREATEBUFFER;
    }

    setChannelMap(device, outputchannels, speakermode ? *speakermode : FMOD_SPEAKERMODE_RAW);

    callback.inputProc       = &OutputCoreAudio::renderCallback;
    callback.inputProcRefCon = this;

    if (AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(AURenderCallbackStruct)) != noErr)
    {
        close();
        return FMOD_ERR_OUTPUT_INIT;
    }

    if (AudioUnitInitialize(mAudioUnit) != noErr)
    {
        close();
        return FMOD_ERR_OUTPUT_INIT;
    }
    mUnitInitialized = true;

    /*
        Ring buffer of dspnumbuffers blocks that the FMOD mixer thread fills behind the play cursor.
    */
    mBlockAlign          = format.mBytesPerFrame;
    mBufferLengthSamples = (unsigned int)(dspbufferlength * dspnumbuffers);
    mBufferLengthBytes   = mBufferLengthSamples * mBlockAlign;
    __atomic_store_n(&mPlayPositionSamples, 0, __ATOMIC_RELEASE);

    mBuffer = (char *)FMOD_Memory_Calloc(mBufferLengthBytes);
    if (!mBuffer)
    {
        close();
        return FMOD_ERR_MEMORY;
    }

    FLOG((FMOD_DEBUG_LEVEL_ALL, __FILE__, __LINE__, "OutputCoreAudio::initEx", "Opened '%s': %d hz, %d channels, %d bits, %d x %d samples.\n", mDriverName[selecteddriver], *outputrate, outputchannels, bits, dspnumbuffers, dspbufferlength));

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::close()
{
    if (mAudioUnit)
    {
        if (mRunning)
        {
            AudioOutputUnitStop(mAudioUnit);
            mRunning = false;
        }
        if (mUnitInitialized)
        {
            AudioUnitUninitialize(mAudioUnit);
            mUnitInitialized = false;
        }

        AudioComponentInstanceDispose(mAudioUnit);
        mAudioUnit = 0;
    }

    if (mBuffer)
    {
        FMOD_Memory_Free(mBuffer);
        mBuffer = 0;
    }

    mBufferLengthBytes   = 0;
    mBufferLengthSamples = 0;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]
    Returns the AudioUnit.

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::getHandle(void **handle)
{
    *handle = mAudioUnit;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::start()
{
    FMOD_RESULT result;

    if (!mAudioUnit)
    {
        return FMOD_ERR_UNINITIALIZED;
    }

    result = OutputPolled::start();
    if (result != FMOD_OK)
    {
        return result;
    }

    if (AudioOutputUnitStart(mAudioUnit) != noErr)
    {
        OutputPolled::stop();
        return FMOD_ERR_OUTPUT_DRIVERCALL;
    }

    mRunning = true;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::stop()
{
    if (mAudioUnit && mRunning)
    {
        AudioOutputUnitStop(mAudioUnit);
        mRunning = false;
    }

    return OutputPolled::stop();
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]
    Play cursor in samples, within the ring buffer.

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::getPosition(unsigned int *pcm)
{
    *pcm = __atomic_load_n(&mPlayPositionSamples, __ATOMIC_ACQUIRE);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]
    FMOD_OK

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
FMOD_RESULT OutputCoreAudio::lock(unsigned int offset, unsigned int length, void **ptr1, void **ptr2, unsigned int *len1, unsigned int *len2)
{
    offset %= mBufferLengthBytes;

    if (offset + length > mBufferLengthBytes)
    {
        *ptr1 = mBuffer + offset;
        *ptr2 = mBuffer;
        *len1 = mBufferLengthBytes - offset;
        *len2 = length - (mBufferLengthBytes - offset);
    }
    else
    {
        *ptr1 = mBuffer + offset;
        *ptr2 = 0;
        *len1 = length;
        *len2 = 0;
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]
    AudioUnit render callback, on the CoreAudio IO thread.  Copies from the ring buffer at the play cursor and
    advances it.  No locks and no allocation here.

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]
    Macintosh

	[SEE_ALSO]
]
*/
OSStatus OutputCoreAudio::renderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)inRefCon;
    char            *dest;
    unsigned int     position;
    unsigned int     remaining;

    if (!ioData || !ioData->mNumberBuffers)
    {
        return noErr;
    }

    dest = (char *)ioData->mBuffers[0].mData;

    if (!coreaudio->mBuffer || !coreaudio->mBufferLengthSamples || !dest)
    {
        if (dest)
        {
            FMOD_memset(dest, 0, ioData->mBuffers[0].mDataByteSize);
        }
        *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    remaining = inNumberFrames;
    if (remaining * coreaudio->mBlockAlign > ioData->mBuffers[0].mDataByteSize)
    {
        remaining = ioData->mBuffers[0].mDataByteSize / coreaudio->mBlockAlign;
    }

    position = __atomic_load_n(&coreaudio->mPlayPositionSamples, __ATOMIC_ACQUIRE);

    while (remaining)
    {
        unsigned int frames = coreaudio->mBufferLengthSamples - position;

        if (frames > remaining)
        {
            frames = remaining;
        }

        FMOD_memcpy(dest, coreaudio->mBuffer + position * coreaudio->mBlockAlign, frames * coreaudio->mBlockAlign);

        dest      += frames * coreaudio->mBlockAlign;
        remaining -= frames;
        position  += frames;
        if (position >= coreaudio->mBufferLengthSamples)
        {
            position = 0;
        }
    }

    __atomic_store_n(&coreaudio->mPlayPositionSamples, position, __ATOMIC_RELEASE);

    return noErr;
}


/*
    Callback wrappers.
*/
FMOD_RESULT F_CALLBACK OutputCoreAudio::getNumDriversCallback(FMOD_OUTPUT_STATE *output, int *numdrivers)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->getNumDrivers(numdrivers);
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::getDriverNameCallback(FMOD_OUTPUT_STATE *output, int id, char *name, int namelen)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->getDriverName(id, name, namelen);
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::getDriverCapsExCallback(FMOD_OUTPUT_STATE *output, int id, FMOD_CAPS *caps, int *minfrequency, int *maxfrequency, FMOD_SPEAKERMODE *controlpanelspeakermode)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->getDriverCapsEx(id, caps, minfrequency, maxfrequency, controlpanelspeakermode);
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::initExCallback(FMOD_OUTPUT_STATE *output, int selecteddriver, FMOD_INITFLAGS flags, int *outputrate, int outputchannels, FMOD_SOUND_FORMAT *outputformat, FMOD_SPEAKERMODE *speakermode, int dspbufferlength, int dspnumbuffers, int max2dchannels, int max3dchannels, void *extradriverdata)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->initEx(selecteddriver, flags, outputrate, outputchannels, outputformat, speakermode, dspbufferlength, dspnumbuffers, extradriverdata);
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::closeCallback(FMOD_OUTPUT_STATE *output)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->close();
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::startCallback(FMOD_OUTPUT_STATE *output)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->start();
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::stopCallback(FMOD_OUTPUT_STATE *output)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->stop();
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::getHandleCallback(FMOD_OUTPUT_STATE *output, void **handle)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->getHandle(handle);
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::getPositionCallback(FMOD_OUTPUT_STATE *output, unsigned int *pcm)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->getPosition(pcm);
}


FMOD_RESULT F_CALLBACK OutputCoreAudio::lockCallback(FMOD_OUTPUT_STATE *output, unsigned int offset, unsigned int length, void **ptr1, void **ptr2, unsigned int *len1, unsigned int *len2)
{
    OutputCoreAudio *coreaudio = (OutputCoreAudio *)output;

    return coreaudio->lock(offset, length, ptr1, ptr2, len1, len2);
}

}   /* namespace FMOD */


/*
    Device list change notification.  A listener on kAudioHardwarePropertyDevices bumps a generation counter on a
    dispatch queue, FMOD_OS_CheckDriverList (called from System::update) compares it with the last one it saw.
*/
static unsigned int gCoreAudioDeviceListGeneration;
static unsigned int gCoreAudioDeviceListSeen;

FMOD_RESULT FMOD_OS_CoreAudio_CheckDriverList(bool *devicelistchanged)
{
    static dispatch_once_t once;
    unsigned int           generation;

    dispatch_once(&once, ^{
        AudioObjectPropertyAddress address = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &address, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
            ^(UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses)
            {
                __atomic_add_fetch(&gCoreAudioDeviceListGeneration, 1, __ATOMIC_RELEASE);
            });
    });

    generation = __atomic_load_n(&gCoreAudioDeviceListGeneration, __ATOMIC_ACQUIRE);

    *devicelistchanged = (generation != gCoreAudioDeviceListSeen);
    gCoreAudioDeviceListSeen = generation;

    return FMOD_OK;
}

#endif  /* FMOD_SUPPORT_COREAUDIO */
