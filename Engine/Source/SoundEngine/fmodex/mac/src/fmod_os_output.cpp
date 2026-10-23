/*
    FMOD Ex output registration for macOS (wowdecomp port). Counterpart of win32/src/fmod_os_output.cpp.
    CoreAudio is the default output; NOSOUND and the WAV writer are registered by the plugin factory itself.
*/

#include "fmod_settings.h"

#include "fmod.h"
#include "fmod_output.h"
#include "fmod_output_coreaudio.h"
#include "fmod_pluginfactory.h"

FMOD_RESULT FMOD_OS_Output_Register(FMOD::PluginFactory *pluginfactory)
{
#ifdef FMOD_SUPPORT_COREAUDIO
    pluginfactory->registerOutput(FMOD::OutputCoreAudio::getDescriptionEx());
#endif

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Output_GetDefault(FMOD_OUTPUTTYPE *outputtype)
{
    if (!outputtype)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

#ifdef FMOD_SUPPORT_COREAUDIO
    *outputtype = FMOD_OUTPUTTYPE_COREAUDIO;
#else
    *outputtype = FMOD_OUTPUTTYPE_NOSOUND;
#endif

    return FMOD_OK;
}
