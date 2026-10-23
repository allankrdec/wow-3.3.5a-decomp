/*
    FMOD Ex OS layer for macOS (wowdecomp port). POSIX counterpart of win32/src/fmod_os_misc.cpp, with the same
    results for the same situations.
*/

#include "fmod_settings.h"

#include "fmod.h"
#include "fmod_memory.h"
#include "fmod_os_misc.h"
#include "fmod_output_coreaudio.h"

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct FMOD_OS_CRITICALSECTION
{
    pthread_mutex_t mutex;
};

static FMOD_OS_CRITICALSECTION gMemoryCrit;
static bool                    gMemoryCritInitialized;

static unsigned long long FMOD_OS_Mac_GetNs()
{
    static mach_timebase_info_data_t timebase;

    if (!timebase.denom)
    {
        mach_timebase_info(&timebase);
    }

    return mach_absolute_time() * timebase.numer / timebase.denom;
}

void *FMOD_OS_Memory_Alloc(int size, FMOD_MEMORY_TYPE type)
{
    return malloc(size);
}

void *FMOD_OS_Memory_Realloc(void *ptr, int size, FMOD_MEMORY_TYPE type)
{
    return realloc(ptr, size);
}

void FMOD_OS_Memory_Free(void *ptr, FMOD_MEMORY_TYPE type)
{
    free(ptr);
}

FMOD_RESULT FMOD_OS_File_DriveStatus()
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Open(const char *name, char *mode, int unicode, unsigned int *filesize, void **handle)
{
    /* Unicode names are UTF-16 on Windows; the client never passes them. */
    if (unicode)
    {
        return FMOD_ERR_FILE_NOTFOUND;
    }

    FILE *file = fopen(name, "rb");

    if (!file)
    {
        if (errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
        {
            return FMOD_ERR_FILE_BAD;
        }
        return FMOD_ERR_FILE_NOTFOUND;
    }

    fseek(file, 0, SEEK_END);
    *filesize = (unsigned int)ftell(file);
    fseek(file, 0, SEEK_SET);

    *handle = file;
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Close(void *handle)
{
    if (!handle)
    {
        return FMOD_ERR_FILE_BAD;
    }

    fclose((FILE *)handle);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Cancel(void *handle)
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Read(void *handle, void *buf, unsigned int count, unsigned int *read)
{
    size_t rd = fread(buf, 1, count, (FILE *)handle);

    if (rd != count && ferror((FILE *)handle))
    {
        return FMOD_ERR_FILE_BAD;
    }

    *read = (unsigned int)rd;

    if (rd != count)
    {
        return FMOD_ERR_FILE_EOF;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Seek(void *handle, unsigned int offset)
{
    if (fseek((FILE *)handle, offset, SEEK_SET))
    {
        return FMOD_ERR_FILE_BAD;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Debug_OutputStr(const char *s)
{
    fputs(s, stderr);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Time_GetNs(unsigned int *ns)
{
    /* Like the Windows version, this is microseconds truncated to 32 bits despite the name. */
    *ns = (unsigned int)(FMOD_OS_Mac_GetNs() / 1000);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Time_GetMs(unsigned int *ms)
{
    *ms = (unsigned int)(FMOD_OS_Mac_GetNs() / 1000000);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Time_Sleep(unsigned int ms)
{
    usleep(ms * 1000);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Thread_GetCurrentID(FMOD_UINT_NATIVE *id)
{
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    *id = (FMOD_UINT_NATIVE)tid;
    return FMOD_OK;
}

struct FMOD_OS_Mac_ThreadStart
{
    THREAD_RETURNTYPE (THREAD_CALLCONV *func)(void *param);
    void *param;
    FMOD_THREAD_PRIORITY priority;
    char name[64];
};

static void *FMOD_OS_Mac_ThreadProc(void *arg)
{
    FMOD_OS_Mac_ThreadStart start = *(FMOD_OS_Mac_ThreadStart *)arg;
    free(arg);

    pthread_setname_np(start.name);

    if (start.priority >= FMOD_THREAD_PRIORITY_HIGH)
    {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
    else if (start.priority <= FMOD_THREAD_PRIORITY_LOW)
    {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    }

    start.func(start.param);
    return NULL;
}

FMOD_RESULT FMOD_OS_Thread_Create(const char *name, THREAD_RETURNTYPE (THREAD_CALLCONV *func)(void *param), void *param, FMOD_THREAD_PRIORITY priority, void *stack, int stacksize, void **handle)
{
    if (!handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    FMOD_OS_Mac_ThreadStart *start = (FMOD_OS_Mac_ThreadStart *)malloc(sizeof(FMOD_OS_Mac_ThreadStart));
    if (!start)
    {
        return FMOD_ERR_MEMORY;
    }
    start->func = func;
    start->param = param;
    start->priority = priority;
    snprintf(start->name, sizeof(start->name), "FMOD %s", name ? name : "");

    /* Windows ignores 'stacksize' (CreateThread with 0) and gets the executable's default 1 MB stack; the callers'
       requests are too small for their real use, and macOS threads default to 512 KB. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);

    pthread_t thread;
    int err = pthread_create(&thread, &attr, FMOD_OS_Mac_ThreadProc, start);
    pthread_attr_destroy(&attr);

    if (err)
    {
        free(start);
        return FMOD_ERR_MEMORY;
    }

    *handle = (void *)thread;
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Thread_Destroy(void *handle)
{
    /* Windows only closes the handle; the thread has already been told to exit and may still be running. */
    pthread_detach((pthread_t)handle);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Create(FMOD_OS_CRITICALSECTION **crit, bool memorycrit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (memorycrit)
    {
        /* Can't use alloc on the critical section used in the memory system. */
        *crit = &gMemoryCrit;
        if (gMemoryCritInitialized)
        {
            return FMOD_OK;
        }
        gMemoryCritInitialized = true;
    }
    else
    {
        *crit = (FMOD_OS_CRITICALSECTION *)FMOD_Memory_Alloc(sizeof(FMOD_OS_CRITICALSECTION));
        if (!*crit)
        {
            return FMOD_ERR_MEMORY;
        }
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&(*crit)->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Free(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (crit == &gMemoryCrit)
    {
        return FMOD_OK;
    }

    pthread_mutex_destroy(&crit->mutex);
    FMOD_Memory_Free(crit);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Enter(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&crit->mutex);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Leave(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_unlock(&crit->mutex);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Semaphore_Create(FMOD_OS_SEMAPHORE **sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    if (!semaphore)
    {
        return FMOD_ERR_MEMORY;
    }

    *sema = (FMOD_OS_SEMAPHORE *)semaphore;
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Semaphore_Free(FMOD_OS_SEMAPHORE *sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_release((dispatch_semaphore_t)sema);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Semaphore_Wait(FMOD_OS_SEMAPHORE *sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_semaphore_wait((dispatch_semaphore_t)sema, DISPATCH_TIME_FOREVER);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Semaphore_Signal(FMOD_OS_SEMAPHORE *sema, bool interrupt)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_semaphore_signal((dispatch_semaphore_t)sema);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Library_Load(const char *dllname, FMOD_OS_LIBRARY **handle)
{
    if (!dllname || !handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *handle = (FMOD_OS_LIBRARY *)dlopen(dllname, RTLD_NOW | RTLD_LOCAL);
    if (!*handle)
    {
        return FMOD_ERR_FILE_NOTFOUND;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Library_GetProcAddress(FMOD_OS_LIBRARY *handle, const char *procname, void **address)
{
    if (!handle || !procname || !address)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *address = dlsym(handle, procname);
    if (!*address)
    {
        return FMOD_ERR_FILE_BAD;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Library_Free(FMOD_OS_LIBRARY *handle)
{
    if (!handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dlclose(handle);
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CheckDriverList(bool *devicelistchanged)
{
#ifdef FMOD_SUPPORT_COREAUDIO
    return FMOD_OS_CoreAudio_CheckDriverList(devicelistchanged);
#else
    *devicelistchanged = false;
    return FMOD_OK;
#endif
}
