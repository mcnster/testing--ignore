/*
 * Copyright (C) 2006 Robert Reif
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "port.h"
#include "common.h"

//#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>

#include <wine/windows/windef.h>
#include <wine/windows/winbase.h>
#include <wine/windows/objbase.h>
#include <wine/windows/mmsystem.h>

#include <sched.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "wine/library.h"
#include "wine/debug.h"

#define IEEE754_64FLOAT 1
#include "asio.h"

WINE_DEFAULT_DEBUG_CHANNEL(asio);

/* WIN32 callback function */
static DWORD CALLBACK win32_callback(LPVOID arg);

/* {48D0C522-BFCC-45cc-8B84-17F25F33E6E8} */
static GUID const CLSID_WineASIO = {
0x48d0c522, 0xbfcc, 0x45cc, { 0x8b, 0x84, 0x17, 0xf2, 0x5f, 0x33, 0xe6, 0xe8 } };

#define twoRaisedTo32           4294967296.0
#define twoRaisedTo32Reciprocal	(1.0 / twoRaisedTo32)

/* ASIO drivers use the thiscall calling convention which only Microsoft compilers
 * produce.  These macros add an extra layer to fixup the registers properly for
 * this calling convention.
 */

#ifdef __i386__  /* thiscall functions are i386-specific */

#ifdef __GNUC__
/* GCC erroneously warns that the newly wrapped function
 * isn't used, lets help it out of it's thinking
 */
#define SUPPRESS_NOTUSED __attribute__((used))
#else
#define SUPPRESS_NOTUSED
#endif /* __GNUC__ */

#define WRAP_THISCALL(type, func, parm) \
    extern type func parm; \
    __ASM_GLOBAL_FUNC( func, \
                      "popl %eax\n\t" \
                      "pushl %ecx\n\t" \
                      "pushl %eax\n\t" \
                      "jmp " __ASM_NAME("__wrapped_" #func) ); \
    SUPPRESS_NOTUSED static type __wrapped_ ## func parm
#else
#define WRAP_THISCALL(functype, function, param) \
    functype function param
#endif

/*****************************************************************************
 * IWineAsio interface
 */
#define INTERFACE IWineASIO
DECLARE_INTERFACE_(IWineASIO,IUnknown)
{
    STDMETHOD_(HRESULT,QueryInterface)(THIS_ REFIID riid, void** ppvObject) PURE;
    STDMETHOD_(ULONG,AddRef)(THIS) PURE;
    STDMETHOD_(ULONG,Release)(THIS) PURE;
    STDMETHOD_(long,init)(THIS_ void *sysHandle) PURE;
    STDMETHOD_(void,getDriverName)(THIS_ char *name) PURE;
    STDMETHOD_(long,getDriverVersion)(THIS) PURE;
    STDMETHOD_(void,getErrorMessage)(THIS_ char *string) PURE;
    STDMETHOD_(ASIOError,start)(THIS) PURE;
    STDMETHOD_(ASIOError,stop)(THIS) PURE;
    STDMETHOD_(ASIOError,getChannels)(THIS_ long *numInputChannels, long *numOutputChannels) PURE;
    STDMETHOD_(ASIOError,getLatencies)(THIS_ long *inputLatency, long *outputLatency) PURE;
    STDMETHOD_(ASIOError,getBufferSize)(THIS_ long *minSize, long *maxSize, long *preferredSize, long *granularity) PURE;
    STDMETHOD_(ASIOError,canSampleRate)(THIS_ ASIOSampleRate sampleRate) PURE;
    STDMETHOD_(ASIOError,getSampleRate)(THIS_ ASIOSampleRate *sampleRate) PURE;
    STDMETHOD_(ASIOError,setSampleRate)(THIS_ ASIOSampleRate sampleRate) PURE;
    STDMETHOD_(ASIOError,getClockSources)(THIS_ ASIOClockSource *clocks, long *numSources) PURE;
    STDMETHOD_(ASIOError,setClockSource)(THIS_ long reference) PURE;
    STDMETHOD_(ASIOError,getSamplePosition)(THIS_ ASIOSamples *sPos, ASIOTimeStamp *tStamp) PURE;
    STDMETHOD_(ASIOError,getChannelInfo)(THIS_ ASIOChannelInfo *info) PURE;
    STDMETHOD_(ASIOError,createBuffers)(THIS_ ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) PURE;
    STDMETHOD_(ASIOError,disposeBuffers)(THIS) PURE;
    STDMETHOD_(ASIOError,controlPanel)(THIS) PURE;
    STDMETHOD_(ASIOError,future)(THIS_ long selector,void *opt) PURE;
    STDMETHOD_(ASIOError,outputReady)(THIS) PURE;
};
#undef INTERFACE

typedef struct IWineASIO *LPWINEASIO, **LPLPWINEASIO;

enum
{
    Init,
    Run,
    Exit
};

typedef struct _Channel {
   ASIOBool active;
   int *buffer;
} Channel;

struct IWineASIOImpl
{
    /* COM stuff */
    const IWineASIOVtbl *lpVtbl;
    LONG                ref;

    /* ASIO stuff */
    HWND                hwnd;
    ASIOSampleRate      sample_rate;
    long                input_latency;
    long                output_latency;
    long                block_frames;
    ASIOTime            asio_time;
    long                miliseconds;
    ASIOTimeStamp       system_time;
    double              sample_position;
    ASIOBufferInfo      *bufferInfos;
    ASIOCallbacks       *callbacks;
    char                error_message[256];
    BOOL                time_info_mode;
    BOOL                tc_read;
    long                state;

    /* pointer to start of shared memory buffer */
    unsigned int        inputs;
    unsigned int        outputs;
    InfoBlock           *infoblock;
    float               *inputblock;
    float               *outputblock;
    long                toggle;

    /* callback stuff */
    HANDLE              thread;
    DWORD               thread_id;
    sem_t               *semaphore1;
    sem_t               *semaphore2;
    BOOL                terminate;
    Channel             *input;
    Channel             *output;
} This;

typedef struct IWineASIOImpl              IWineASIOImpl;

static ULONG WINAPI IWineASIOImpl_AddRef(LPWINEASIO iface)
{
    ULONG ref = InterlockedIncrement(&(This.ref));
    TRACE("(%p) ref was %x\n", &This, ref - 1);
    return ref;
}

static ULONG WINAPI IWineASIOImpl_Release(LPWINEASIO iface)
{
    ULONG ref = InterlockedDecrement(&(This.ref));
    TRACE("(%p) ref was %x\n", &This, ref + 1);

    if (!ref) {

        This.terminate = TRUE;

    }

    return ref;
}

static HRESULT WINAPI IWineASIOImpl_QueryInterface(LPWINEASIO iface, REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_INVALIDARG;

    if (IsEqualIID(&CLSID_WineASIO, riid))
    {

        This.ref++;
        *ppvObject = &This;

        return S_OK;
    }

    return E_NOINTERFACE;
}

WRAP_THISCALL( ASIOBool __stdcall, IWineASIOImpl_init, (LPWINEASIO iface, void *sysHandle))
{
    int i,j;
    int handle;

    float *memblock;

    if ((handle = shm_open("wineasio-info", O_RDWR, 0666)) == -1) 
    {
       WARN("failed to open shm info. Is jack client running?\n");
       return ASIOFalse;
    }

    ftruncate(handle, sizeof(InfoBlock));
    This.infoblock = (InfoBlock *)mmap(0, sizeof(InfoBlock),
                                   PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);

    close(handle);

    This.sample_rate = (double)This.infoblock->sample_rate;
    This.block_frames = This.infoblock->buffer_frames;
    This.inputs = This.infoblock->inputs;
    This.outputs = This.infoblock->outputs;

    TRACE("info block allocated");

    if ((handle = shm_open("wineasio-buffers", O_RDWR, 0666)) == -1)
    {
       WARN("failed to open shm buffers. Is jack client running?\n");
       return ASIOFalse;
    }
    
    ftruncate(handle, sizeof(float)* This.block_frames * (This.inputs + This.outputs));
    memblock = (float *)mmap(0, 
                sizeof(float) * This.block_frames * (This.inputs + This.outputs),
                PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    close(handle);

    TRACE("mem block allocated");

    This.inputblock = memblock;
    This.outputblock = memblock + This.inputs * This.block_frames;

    printf("in = %p, out = %p\n", This.inputblock, This.outputblock);

    This.input_latency = This.block_frames;
    This.output_latency = This.block_frames * 2;
    This.miliseconds = (long)((double)(This.block_frames * 1000) / This.sample_rate);
    This.callbacks = NULL;
    This.sample_position = 0;
    strcpy(This.error_message, "No Error");
    This.toggle = 1;
    This.time_info_mode = FALSE;
    This.tc_read = FALSE;
    This.terminate = FALSE;
    This.state = Init;

    TRACE("sample rate: %f\n", This.sample_rate);

    // initialize input buffers

    This.input = HeapAlloc(GetProcessHeap(), 0, sizeof(Channel) * This.inputs);
    for (i=0; i<This.inputs; i++) {
        
        This.input[i].active = ASIOFalse;
        This.input[i].buffer = HeapAlloc(GetProcessHeap(), 0, 2 * This.block_frames * sizeof(int));
        for (j=0; j<This.block_frames * 2; j++)
            This.input[i].buffer[j] = 0;
    }

    // initialize output buffers

    This.output =  HeapAlloc(GetProcessHeap(), 0, sizeof(Channel) * This.outputs);
    for (i=0; i<This.outputs; i++) {
        This.output[i].active = ASIOFalse;
        This.output[i].buffer = HeapAlloc(GetProcessHeap(), 0, 2 * This.block_frames * sizeof(int));
        for (j=0; j<2*This.block_frames; j++)
            This.output[i].buffer[j] = 0;
    }

    This.semaphore1 = sem_open("wineasio-sem1", O_RDWR);
    This.semaphore2 = sem_open("wineasio-sem2", O_RDWR);

    This.thread = CreateThread(NULL, 0, win32_callback, &This, 0, &This.thread_id);
    return ASIOTrue;
}

WRAP_THISCALL( void __stdcall, IWineASIOImpl_getDriverName, (LPWINEASIO iface, char *name))
{
    strcpy(name, "Wine ASIO");
}

WRAP_THISCALL( long __stdcall, IWineASIOImpl_getDriverVersion, (LPWINEASIO iface))
{
    return 2;
}

WRAP_THISCALL( void __stdcall, IWineASIOImpl_getErrorMessage, (LPWINEASIO iface, char *string))
{
    strcpy(string, This.error_message);
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_start, (LPWINEASIO iface))
{

    if (This.callbacks)
    {
        This.sample_position = 0;
        This.system_time.lo = 0;
        This.system_time.hi = 0;

        This.state = Run;
        TRACE("started\n");

        return ASE_OK;
    }

    return ASE_NotPresent;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_stop, (LPWINEASIO iface))
{

    This.state = Exit;

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getChannels, (LPWINEASIO iface, long *numInputChannels, long *numOutputChannels))
{

    if (numInputChannels)
        *numInputChannels = This.inputs;

    if (numOutputChannels)
        *numOutputChannels = This.outputs;

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getLatencies, (LPWINEASIO iface, long *inputLatency, long *outputLatency))
{

    if (inputLatency)
        *inputLatency = This.input_latency;

    if (outputLatency)
        *outputLatency = This.output_latency;

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getBufferSize, (LPWINEASIO iface, long *minSize, long *maxSize, long *preferredSize, long *granularity))
{

    if (minSize)
        *minSize = This.block_frames;

    if (maxSize)
        *maxSize = This.block_frames;

    if (preferredSize)
        *preferredSize = This.block_frames;

    if (granularity)
        *granularity = 0;

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_canSampleRate, (LPWINEASIO iface, ASIOSampleRate sampleRate))
{

    if (sampleRate == This.sample_rate)
        return ASE_OK;

    return ASE_NoClock;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getSampleRate, (LPWINEASIO iface, ASIOSampleRate *sampleRate))
{

    if (sampleRate)
        *sampleRate = This.sample_rate;

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_setSampleRate, (LPWINEASIO iface, ASIOSampleRate sampleRate))
{

    if (sampleRate != This.sample_rate)
        return ASE_NoClock;

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getClockSources, (LPWINEASIO iface, ASIOClockSource *clocks, long *numSources))
{

    if (clocks && numSources)
    {
        clocks->index = 0;
        clocks->associatedChannel = -1;
        clocks->associatedGroup = -1;
        clocks->isCurrentSource = ASIOTrue;
        strcpy(clocks->name, "Internal");

        *numSources = 1;
        return ASE_OK;
    }

    return ASE_InvalidParameter;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_setClockSource, (LPWINEASIO iface, long reference))
{

    if (reference == 0)
    {
        This.asio_time.timeInfo.flags |= kClockSourceChanged;

        return ASE_OK;
    }

    return ASE_NotPresent;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getSamplePosition, (LPWINEASIO iface, ASIOSamples *sPos, ASIOTimeStamp *tStamp))
{

    tStamp->lo = This.system_time.lo;
    tStamp->hi = This.system_time.hi;

    if (This.sample_position >= twoRaisedTo32)
    {
        sPos->hi = (unsigned long)(This.sample_position * twoRaisedTo32Reciprocal);
        sPos->lo = (unsigned long)(This.sample_position - (sPos->hi * twoRaisedTo32));
    }
    else
    {
        sPos->hi = 0;
        sPos->lo = (unsigned long)This.sample_position;
    }

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_getChannelInfo, (LPWINEASIO iface, ASIOChannelInfo *info))
{
    char name[32];

    if (info->channel < 0 || (info->isInput ? info->channel >= This.inputs : info->channel >= This.outputs))
        return ASE_InvalidParameter;

//    info->type = ASIOSTFloat32LSB;
    info->type = ASIOSTInt32LSB;
    info->channelGroup = 0;

    if (info->isInput)
    {
        info->isActive = This.input[info->channel].active;
        sprintf(name, "Input %ld", info->channel);
    }
    else
    {
        info->isActive = This.output[info->channel].active;
        sprintf(name, "Output %ld", info->channel);
    }

    strcpy(info->name, name);

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_disposeBuffers, (LPWINEASIO iface))
{
    int i;

    This.callbacks = NULL;
    __wrapped_IWineASIOImpl_stop(iface);

    for (i = 0; i < This.inputs; i++)
    {
        This.input[i].active = ASIOFalse;
    }

    for (i = 0; i < This.outputs; i++)
    {
        This.output[i].active = ASIOFalse;
    }

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_createBuffers, (LPWINEASIO iface, ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks))
{
    ASIOBufferInfo * info = bufferInfos;
    int i;

    This.block_frames = bufferSize;
    This.miliseconds = (long)((double)(This.block_frames * 1000) / This.sample_rate);

    for (i = 0; i < numChannels; i++, info++)
    {
        if (info->isInput)
        {
            if (info->channelNum < 0 || info->channelNum >= This.inputs)
            {
                WARN("invalid input channel: %ld\n", info->channelNum);
                goto ERROR_PARAM;
            }

            This.input[info->channelNum].active = ASIOTrue;
            info->buffers[0] = &This.input[info->channelNum].buffer[0];
            info->buffers[1] = &This.input[info->channelNum].buffer[This.block_frames];

        }
        else
        {
            if (info->channelNum < 0 || info->channelNum >= This.outputs)
            {
                WARN("invalid output channel: %ld\n", info->channelNum);
                goto ERROR_PARAM;
            }

            This.output[info->channelNum].active = ASIOTrue;
            info->buffers[0] = &This.output[info->channelNum].buffer[0];
            info->buffers[1] = &This.output[info->channelNum].buffer[This.block_frames];

        }
    }

    This.callbacks = callbacks;

    if (This.callbacks->asioMessage)
    {
        if (This.callbacks->asioMessage(kAsioSupportsTimeInfo, 0, 0, 0))
        {
            This.time_info_mode = TRUE;
            This.asio_time.timeInfo.speed = 1;
            This.asio_time.timeInfo.systemTime.hi = 0;
            This.asio_time.timeInfo.systemTime.lo = 0;
            This.asio_time.timeInfo.samplePosition.hi = 0;
            This.asio_time.timeInfo.samplePosition.lo = 0;
            This.asio_time.timeInfo.sampleRate = This.sample_rate;
            This.asio_time.timeInfo. flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;

            This.asio_time.timeCode.speed = 1;
            This.asio_time.timeCode.timeCodeSamples.hi = 0;
            This.asio_time.timeCode.timeCodeSamples.lo = 0;
            This.asio_time.timeCode.flags = kTcValid | kTcRunning;
        }
        else
            This.time_info_mode = FALSE;
    }
    else
    {
        This.time_info_mode = FALSE;
        WARN("asioMessage callback not supplied\n");
        goto ERROR_PARAM;
    }

    return ASE_OK;

ERROR_PARAM:
    __wrapped_IWineASIOImpl_disposeBuffers(iface);
    WARN("invalid parameter\n");
    return ASE_InvalidParameter;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_controlPanel, (LPWINEASIO iface))
{

    return ASE_OK;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_future, (LPWINEASIO iface, long selector, void *opt))
{

    switch (selector)
    {
    case kAsioEnableTimeCodeRead:
        This.tc_read = TRUE;
        return ASE_SUCCESS;
    case kAsioDisableTimeCodeRead:
        This.tc_read = FALSE;
        return ASE_SUCCESS;
    case kAsioSetInputMonitor:
        return ASE_SUCCESS;
    case kAsioCanInputMonitor:
        return ASE_SUCCESS;
    case kAsioCanTimeInfo:
        return ASE_SUCCESS;
    case kAsioCanTimeCode:
        return ASE_SUCCESS;
    }

    return ASE_NotPresent;
}

WRAP_THISCALL( ASIOError __stdcall, IWineASIOImpl_outputReady, (LPWINEASIO iface))
{
    return ASE_NotPresent;
}

static const IWineASIOVtbl WineASIO_Vtbl =
{
    IWineASIOImpl_QueryInterface,
    IWineASIOImpl_AddRef,
    IWineASIOImpl_Release,
    IWineASIOImpl_init,
    IWineASIOImpl_getDriverName,
    IWineASIOImpl_getDriverVersion,
    IWineASIOImpl_getErrorMessage,
    IWineASIOImpl_start,
    IWineASIOImpl_stop,
    IWineASIOImpl_getChannels,
    IWineASIOImpl_getLatencies,
    IWineASIOImpl_getBufferSize,
    IWineASIOImpl_canSampleRate,
    IWineASIOImpl_getSampleRate,
    IWineASIOImpl_setSampleRate,
    IWineASIOImpl_getClockSources,
    IWineASIOImpl_setClockSource,
    IWineASIOImpl_getSamplePosition,
    IWineASIOImpl_getChannelInfo,
    IWineASIOImpl_createBuffers,
    IWineASIOImpl_disposeBuffers,
    IWineASIOImpl_controlPanel,
    IWineASIOImpl_future,
    IWineASIOImpl_outputReady,
};

HRESULT asioCreateInstance(REFIID riid, LPVOID *ppobj)
{
    This.lpVtbl = &WineASIO_Vtbl;
    This.ref = 1;
    *ppobj = &This;
    return S_OK;
}

static void getNanoSeconds(ASIOTimeStamp* ts)
{
    double nanoSeconds = (double)((unsigned long)timeGetTime ()) * 1000000.;
    ts->hi = (unsigned long)(nanoSeconds / twoRaisedTo32);
    ts->lo = (unsigned long)(nanoSeconds - (ts->hi * twoRaisedTo32));
}

/*
 * The ASIO callback can make WIN32 calls which require a WIN32 thread.
 * Do the callback in this thread and then switch back to the Jack callback thread.
 */
static DWORD CALLBACK win32_callback(LPVOID arg)
{

    int i, j;
    float *in, *out;
    int *buffer;

    struct sched_param attr;

    attr.__sched_priority = This.infoblock->priority-1;
    sched_setscheduler(0, SCHED_FIFO, &attr);

    while (1)
    {
        /* wait to be woken up by the Jack callback thread */
        This.infoblock->running = 1;
        sem_wait(This.semaphore1);

        if (This.infoblock->transport_rolling) {

           This.asio_time.timeCode.flags =
             This.asio_time.timeCode.flags | kTcRunning;
        }
        else {
           This.asio_time.timeCode.flags = This.asio_time.timeCode.flags & ~kTcRunning;
        } 

        This.sample_position = This.infoblock->frame;

        /* check for termination */
        if (This.terminate)
        {
            This.infoblock->running = 0;
            unlink("wineasio-buffers");
            unlink("wineasio-info");
            sem_post(This.semaphore2);
            return 0;
        }
        getNanoSeconds(&This.system_time);

        /* make sure we are in the run state */

        if (This.state == Run)
        {

           memset(This.outputblock, 0, 
                    sizeof(float) * This.block_frames * This.outputs);
                    
           /* get the input data from JACK and copy it to the ASIO buffers */
           for (i = 0; i < This.inputs; i++)
           {
               if (This.input[i].active == ASIOTrue) {

                  buffer = &This.input[i].buffer[This.block_frames * This.toggle];
                  in = This.inputblock + i * This.block_frames;

                  for (j = 0; j < This.block_frames; j++)
                      buffer[j] = (int)(in[j] * (float)(0x7fffffff));

               }
            }

            if (This.time_info_mode)
            {
                __wrapped_IWineASIOImpl_getSamplePosition((LPWINEASIO)&This,
                    &This.asio_time.timeInfo.samplePosition, &This.asio_time.timeInfo.systemTime);
                if (This.tc_read)
                {
                    This.asio_time.timeCode.timeCodeSamples.lo = This.asio_time.timeInfo.samplePosition.lo;
                    This.asio_time.timeCode.timeCodeSamples.hi = 0;
                }
                This.callbacks->bufferSwitchTimeInfo(&This.asio_time, This.toggle, ASIOTrue);
                This.asio_time.timeInfo.flags &= ~(kSampleRateChanged | kClockSourceChanged);
            }
            else {
                This.callbacks->bufferSwitch(This.toggle, ASIOTrue);
            }

            for (i = 0; i < This.outputs; i++)
            {
                if (This.output[i].active == ASIOTrue) {

                   buffer = &This.output[i].buffer[This.block_frames * (This.toggle ? 0 : 1)];
                   out = This.outputblock + i * This.block_frames;

                   for (j = 0; j < This.block_frames; j++)
                       out[j] = ((float)(buffer[j]) / (float)(0x7fffffff));

                }
            }

            This.toggle = This.toggle ? 0 : 1;
            
        }

        sem_post(This.semaphore2);
    }
    
    return 0;
}
