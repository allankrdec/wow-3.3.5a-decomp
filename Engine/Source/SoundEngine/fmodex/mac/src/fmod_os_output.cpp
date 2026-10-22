/*
    FMOD Ex output registration for macOS (wowdecomp port). Counterpart of win32/src/fmod_os_output.cpp.
    No platform output is registered yet, so FMOD falls back to the always-available NOSOUND output.
    TODO_MAC (E6): register fmod_output_coreaudio and make it the default.
*/

#include "fmod_settings.h"

#include "fmod.h"
#include "fmod_output.h"
#include "fmod_pluginfactory.h"

FMOD_RESULT FMOD_OS_Output_Register(FMOD::PluginFactory *pluginfactory)
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Output_GetDefault(FMOD_OUTPUTTYPE *outputtype)
{
    if (!outputtype)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *outputtype = FMOD_OUTPUTTYPE_NOSOUND;
    return FMOD_OK;
}
