/*
    FMOD Ex network streaming for macOS (wowdecomp port). Counterpart of win32/src/fmod_os_net.cpp.
    The client never streams sound over the network; every call reports that the network is unavailable.
*/

#include "fmod_settings.h"

#include "fmod.h"
#include "fmod_os_net.h"

FMOD_RESULT FMOD_OS_Net_Init()
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Shutdown()
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Connect(const char *host, const unsigned short port, void **handle)
{
    return FMOD_ERR_NET_CONNECT;
}

FMOD_RESULT FMOD_OS_Net_Listen(const unsigned short port, void **listenhandle)
{
    return FMOD_ERR_NET_SOCKET_ERROR;
}

FMOD_RESULT FMOD_OS_Net_Accept(const void *listenhandle, void **handle)
{
    return FMOD_ERR_NET_SOCKET_ERROR;
}

FMOD_RESULT FMOD_OS_Net_Close(const void *handle)
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Write(const void *handle, const char *buf, const unsigned int len, unsigned int *byteswritten)
{
    return FMOD_ERR_NET_SOCKET_ERROR;
}

FMOD_RESULT FMOD_OS_Net_Read(const void *handle, char *buf, const unsigned int len, unsigned int *bytesread)
{
    return FMOD_ERR_NET_SOCKET_ERROR;
}

FMOD_RESULT FMOD_OS_Net_ReadLine(const void *handle, char *buf, const unsigned int len)
{
    return FMOD_ERR_NET_SOCKET_ERROR;
}
