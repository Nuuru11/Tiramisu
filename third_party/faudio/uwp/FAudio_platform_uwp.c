/* FAudio UWP platform layer (Tiramisu-only, Path B step 6)
 *
 * FAudio's own Windows backend (FAudio_platform_win32.c) renders through
 * WASAPI via IMMDeviceEnumerator / IMMDevice -- but those interfaces are
 * declared only under WINAPI_PARTITION_DESKTOP | GAMES (mmdeviceapi.h
 * lines 256-907), so that file cannot compile under the host's
 * WINAPI_FAMILY_APP UWP profile. XAudio2 2.8, by contrast, is entirely
 * in the APP partition (xaudio2.h line 23), xaudio2_8.lib is already
 * linked, and src/host/audio.cpp already runs a proven XAudio2 2.8 mixer
 * on this exact build -- so this layer renders FAudio's mix through
 * XAudio2: one shared IXAudio2 engine, one mastering voice + one float
 * source voice per FAudio device, fed by a render thread that mixes
 * FAudio's 10 ms (or 1024-sample) quantum into rotating buffers.
 *
 * XAudio2 2.8 has no device enumeration (GetDeviceCount/GetDeviceDetails
 * were removed in 2.8; the mastering voice always targets the default
 * device), so this layer reports exactly one device: the default. Its
 * format is read for real from a temporary mastering voice
 * (GetVoiceDetails), not assumed.
 *
 * XNA Song playback uses Media Foundation's Source Reader, adapted from
 * FAudio's own win32 backend (MFStartup / MFCreateSourceReaderFromURL /
 * MFCreateMediaType / MFCreateAttributes are all APP-partition APIs).
 * The one desktop-only piece of upstream's song code,
 * PropVariantToInt64 / PropVariantClear (propvarutil.h, DESKTOP-only), is
 * replaced by reading the VT_UI8 duration straight out of the PROPVARIANT
 * union -- same value, no desktop API (VT_UI8 needs no Clear).
 *
 * WMA decoding is NOT built (HAVE_WMADEC off, as in the Linux build):
 * wmadmod.dll does not exist on Xbox. The vendored wmadec file is still
 * compiled for its upstream fallback decode_wma (logs + silence).
 *
 * No thread naming: upstream resolves SetThreadDescription via
 * LoadLibrary + GetProcAddress (a desktop-only idiom) and the name is
 * debugger-cosmetic only, so this layer skips it.
 *
 * The song + file-I/O sections are adapted from FAudio's win32 backend
 * (FAudio_platform_win32.c, zlib license, Ethan Lee and contributors);
 * the XAudio2 render / enumeration code is Tiramisu-original.
 *
 * Console-verification-pending: audible output, the MF DLLs' presence in
 * the Dev Mode AppContainer, and the device format values can only be
 * verified on the console. What this file guarantees offline is that the
 * code compiles under WINAPI_FAMILY_APP, links, and exports FAudio's full
 * entry-point surface from the exe.
 */

#ifdef FAUDIO_UWP_PLATFORM

#include <stddef.h>

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfreadwrite.h>

#define _SPEAKER_POSITIONS_ /* Defined by SDK. */
#include "FAudio_internal.h"

/* Rotating mix buffers per device: at most UWP_FAUDIO_QUEUED buffers are
 * submitted at once, so the slot being mixed is never in flight. */
#define UWP_FAUDIO_BUFFERS 3
#define UWP_FAUDIO_QUEUED 2

void FAudio_Log(char const *msg)
{
	OutputDebugStringA(msg);
}

/* Shared IXAudio2 engine, refcounted like upstream's device enumerator:
 * created on the first FAudio_PlatformAddRef, destroyed on the last
 * FAudio_PlatformRelease. COM is initialized for the same lifetime (MF
 * needs it). A failed XAudio2Create leaves uwp_engine NULL; the platform
 * functions below then fail gracefully (count 0 / INVALID_CALL / NULL
 * platformDevice), which FNA handles by bailing out of audio init -- the
 * game keeps running without XNA audio instead of crashing. */
static CRITICAL_SECTION faudio_cs = { NULL, -1, 0, 0, 0, 0 };
static IXAudio2 *uwp_engine = NULL;
static LONG uwp_refcount = 0;
static HRESULT uwp_init_hr = S_OK;

void FAudio_PlatformAddRef(void)
{
	EnterCriticalSection(&faudio_cs);
	if (uwp_refcount == 0)
	{
		HRESULT hr;
		/* CoInitializeEx (MTA): plain CoInitialize is not declared
		 * under the APP partition in this SDK, and MTA suits MF use
		 * from the render thread. */
		uwp_init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		hr = XAudio2Create(&uwp_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
		if (FAILED(hr))
		{
			FAudio_Log("FAudio (UWP): XAudio2Create failed, FNA audio unavailable\n");
			uwp_engine = NULL;
		}
	}
	uwp_refcount += 1;
	LeaveCriticalSection(&faudio_cs);
}

void FAudio_PlatformRelease(void)
{
	EnterCriticalSection(&faudio_cs);
	uwp_refcount -= 1;
	if (uwp_refcount <= 0)
	{
		uwp_refcount = 0;
		if (uwp_engine != NULL)
		{
			IXAudio2_Release(uwp_engine);
			uwp_engine = NULL;
		}
		if (SUCCEEDED(uwp_init_hr))
		{
			CoUninitialize();
		}
	}
	LeaveCriticalSection(&faudio_cs);
}

struct FAudioUWPPlatformData
{
	FAudio *audio;
	IXAudio2MasteringVoice *master;
	IXAudio2SourceVoice *src;
	HANDLE renderThread;
	HANDLE stopEvent;
	float *buffers[UWP_FAUDIO_BUFFERS];
	uint32_t updateSize; /* frames per buffer */
	uint32_t channels;
};

/* Mix one quantum into dst (silence when the engine is stopped). */
static void FAudio_UWPMixQuantum(struct FAudioUWPPlatformData *data, float *dst)
{
	FAudio_zero(dst, data->updateSize * data->channels * sizeof(float));
	if (data->audio->active)
	{
		FAudio_INTERNAL_UpdateEngine(data->audio, dst);
	}
}

static void FAudio_UWPSubmitQuantum(struct FAudioUWPPlatformData *data, float *dst)
{
	XAUDIO2_BUFFER xbuf;
	FAudio_zero(&xbuf, sizeof(xbuf));
	xbuf.AudioBytes = data->updateSize * data->channels * sizeof(float);
	xbuf.pAudioData = (const BYTE *) dst;
	xbuf.PlayLength = data->updateSize;
	IXAudio2SourceVoice_SubmitSourceBuffer(data->src, &xbuf, NULL);
}

static DWORD WINAPI FAudio_UWPRenderThread(void *user)
{
	struct FAudioUWPPlatformData *data = user;
	XAUDIO2_VOICE_STATE state;
	int slot = 0;

	/* Two buffers were pre-filled before Start; slot 2 is the first free
	 * one. From here on, keep UWP_FAUDIO_QUEUED buffers ahead. */
	slot = UWP_FAUDIO_QUEUED % UWP_FAUDIO_BUFFERS;
	for (;;)
	{
		if (WaitForSingleObject(data->stopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}
		IXAudio2SourceVoice_GetState(data->src, &state, 0);
		if (state.BuffersQueued < UWP_FAUDIO_QUEUED)
		{
			FAudio_UWPMixQuantum(data, data->buffers[slot]);
			FAudio_UWPSubmitQuantum(data, data->buffers[slot]);
			slot = (slot + 1) % UWP_FAUDIO_BUFFERS;
		}
		else
		{
			/* Queue full: wait for room (or the stop signal). The
			 * quantum is 10 ms, so a 5 ms sleep keeps up. */
			WaitForSingleObject(data->stopEvent, 5);
		}
	}
	return 0;
}

void FAudio_PlatformInit(
	FAudio *audio,
	uint32_t flags,
	uint32_t deviceIndex,
	FAudioWaveFormatExtensible *mixFormat,
	uint32_t *updateSize,
	void** platformDevice
) {
	struct FAudioUWPPlatformData *data;
	WAVEFORMATEXTENSIBLE wfx;
	HRESULT hr;
	int i;
	BOOL has_sse2 = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE);

	FAudio_INTERNAL_InitSIMDFunctions(has_sse2, FALSE);
	FAudio_PlatformAddRef();

	*platformDevice = NULL;

	/* XAudio2 2.8 has one device: the default. Anything else is invalid. */
	if (deviceIndex != 0 || uwp_engine == NULL)
	{
		FAudio_PlatformRelease();
		return;
	}

	data = FAudio_malloc(sizeof(*data));
	if (!data)
	{
		FAudio_PlatformRelease();
		return;
	}
	FAudio_zero(data, sizeof(*data));
	data->audio = audio;
	data->channels = mixFormat->Format.nChannels;

	/* Same quantum as upstream's win32 backend: 10 ms, or ~21.33 ms for
	 * FAUDIO_1024_QUANTUM. Assigned to *updateSize BEFORE any mixing
	 * happens: GenerateOutput mixes audio->updateSize frames, so the
	 * value must be live before the pre-fill and the render thread. */
	if (flags & FAUDIO_1024_QUANTUM)
	{
		data->updateSize = (uint32_t)(
			mixFormat->Format.nSamplesPerSec / (1000.0 / (64.0 / 3.0))
		);
	}
	else
	{
		data->updateSize = mixFormat->Format.nSamplesPerSec / 100;
	}
	*updateSize = data->updateSize;

	/* Mastering voice at the device's native format (0/0 = default).
	 * AudioCategory_GameEffects is the C++ default for the category. */
	hr = IXAudio2_CreateMasteringVoice(
		uwp_engine,
		&data->master,
		0,
		0,
		0,
		NULL,
		NULL,
		AudioCategory_GameEffects
	);
	if (FAILED(hr) || data->master == NULL)
	{
		FAudio_free(data);
		FAudio_PlatformRelease();
		return;
	}

	/* Source voice at the FAudio mix format (float32). XAudio2 resamples
	 * to the mastering voice when the mix rate differs from the device. */
	FAudio_zero(&wfx, sizeof(wfx));
	wfx.Format.wFormatTag = mixFormat->Format.wFormatTag;
	wfx.Format.nChannels = mixFormat->Format.nChannels;
	wfx.Format.nSamplesPerSec = mixFormat->Format.nSamplesPerSec;
	wfx.Format.nAvgBytesPerSec = mixFormat->Format.nAvgBytesPerSec;
	wfx.Format.nBlockAlign = mixFormat->Format.nBlockAlign;
	wfx.Format.wBitsPerSample = mixFormat->Format.wBitsPerSample;
	wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	wfx.Samples.wValidBitsPerSample = mixFormat->Samples.wValidBitsPerSample;
	wfx.dwChannelMask = mixFormat->dwChannelMask;
	FAudio_memcpy(&wfx.SubFormat, &mixFormat->SubFormat, sizeof(GUID));
	hr = IXAudio2_CreateSourceVoice(
		uwp_engine,
		&data->src,
		&wfx.Format,
		0,
		1.0f,
		NULL,
		NULL,
		NULL
	);
	if (FAILED(hr) || data->src == NULL)
	{
		IXAudio2MasteringVoice_DestroyVoice(data->master);
		FAudio_free(data);
		FAudio_PlatformRelease();
		return;
	}

	for (i = 0; i < UWP_FAUDIO_BUFFERS; i += 1)
	{
		data->buffers[i] = FAudio_malloc(
			data->updateSize * data->channels * sizeof(float)
		);
		if (!data->buffers[i])
		{
			int j;
			for (j = 0; j < i; j += 1)
			{
				FAudio_free(data->buffers[j]);
			}
			IXAudio2SourceVoice_DestroyVoice(data->src);
			IXAudio2MasteringVoice_DestroyVoice(data->master);
			FAudio_free(data);
			FAudio_PlatformRelease();
			return;
		}
	}

	data->stopEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!data->stopEvent)
	{
		for (i = 0; i < UWP_FAUDIO_BUFFERS; i += 1)
		{
			FAudio_free(data->buffers[i]);
		}
		IXAudio2SourceVoice_DestroyVoice(data->src);
		IXAudio2MasteringVoice_DestroyVoice(data->master);
		FAudio_free(data);
		FAudio_PlatformRelease();
		return;
	}

	/* Pre-fill two buffers so playback starts without a gap, then start
	 * the voice and hand the queue to the render thread. */
	for (i = 0; i < UWP_FAUDIO_QUEUED; i += 1)
	{
		FAudio_UWPMixQuantum(data, data->buffers[i]);
		FAudio_UWPSubmitQuantum(data, data->buffers[i]);
	}
	IXAudio2SourceVoice_Start(data->src, 0, XAUDIO2_COMMIT_NOW);

	*platformDevice = data;
	data->renderThread = CreateThread(
		NULL, 0, &FAudio_UWPRenderThread, data, 0, NULL
	);
	if (!data->renderThread)
	{
		IXAudio2SourceVoice_Stop(data->src, 0, XAUDIO2_COMMIT_NOW);
		IXAudio2SourceVoice_FlushSourceBuffers(data->src);
		IXAudio2SourceVoice_DestroyVoice(data->src);
		IXAudio2MasteringVoice_DestroyVoice(data->master);
		CloseHandle(data->stopEvent);
		for (i = 0; i < UWP_FAUDIO_BUFFERS; i += 1)
		{
			FAudio_free(data->buffers[i]);
		}
		FAudio_free(data);
		*platformDevice = NULL;
		FAudio_PlatformRelease();
		return;
	}
}

void FAudio_PlatformQuit(void* platformDevice)
{
	struct FAudioUWPPlatformData *data = platformDevice;
	int i;

	if (!data)
	{
		return;
	}
	SetEvent(data->stopEvent);
	WaitForSingleObject(data->renderThread, INFINITE);
	CloseHandle(data->renderThread);
	CloseHandle(data->stopEvent);
	if (data->src)
	{
		IXAudio2SourceVoice_Stop(data->src, 0, XAUDIO2_COMMIT_NOW);
		IXAudio2SourceVoice_FlushSourceBuffers(data->src);
		IXAudio2SourceVoice_DestroyVoice(data->src);
	}
	if (data->master)
	{
		IXAudio2MasteringVoice_DestroyVoice(data->master);
	}
	for (i = 0; i < UWP_FAUDIO_BUFFERS; i += 1)
	{
		FAudio_free(data->buffers[i]);
	}
	FAudio_free(data);
	FAudio_PlatformRelease();
}

uint32_t FAudio_PlatformGetDeviceCount(void)
{
	uint32_t count;
	FAudio_PlatformAddRef();
	count = (uwp_engine != NULL) ? 1 : 0;
	FAudio_PlatformRelease();
	return count;
}

uint32_t FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails *details
) {
	IXAudio2MasteringVoice *master = NULL;
	XAUDIO2_VOICE_DETAILS vd;
	HRESULT hr;

	/* One device: the default (index 0). FNA calls this for every index
	 * below the count, and CreateMasteringVoice with DEFAULT channels /
	 * rate takes the mix format from here -- so the format must be real. */
	if (index != 0 || details == NULL)
	{
		return FAUDIO_E_INVALID_CALL;
	}
	FAudio_PlatformAddRef();
	if (uwp_engine == NULL)
	{
		FAudio_PlatformRelease();
		return FAUDIO_E_INVALID_CALL;
	}

	/* A mastering voice created at 0/0 runs at the device's native mix
	 * format; GetVoiceDetails reads it back for real. */
	hr = IXAudio2_CreateMasteringVoice(
		uwp_engine,
		&master,
		0,
		0,
		0,
		NULL,
		NULL,
		AudioCategory_GameEffects
	);
	if (FAILED(hr) || master == NULL)
	{
		FAudio_PlatformRelease();
		return FAUDIO_E_INVALID_CALL;
	}
	IXAudio2Voice_GetVoiceDetails((IXAudio2Voice *) master, &vd);
	IXAudio2MasteringVoice_DestroyVoice(master);
	FAudio_PlatformRelease();

	if (vd.InputChannels < 1 || vd.InputChannels > 8 ||
		vd.InputSampleRate == 0)
	{
		return FAUDIO_E_INVALID_CALL;
	}

	FAudio_zero(details, sizeof(*details));
	/* DeviceID: no device-ID string is obtainable from the APP partition
	 * without WinRT device enumeration, and FNA does not use DeviceID.
	 * Left empty (documented in uwp/README.md). */
	FAudio_memcpy(
		details->DisplayName,
		L"Default Audio Device",
		sizeof(L"Default Audio Device")
	);
	/* The single default output is the default for every role, including
	 * the Game role FNA looks for first. */
	details->Role = FAudioGlobalDefaultDevice;
	WriteWaveFormatExtensible(
		&details->OutputFormat,
		(int) vd.InputChannels,
		(int) vd.InputSampleRate,
		&DATAFORMAT_SUBTYPE_IEEE_FLOAT
	);
	return 0;
}

FAudioMutex FAudio_PlatformCreateMutex(void)
{
	CRITICAL_SECTION *cs;

	cs = FAudio_malloc(sizeof(CRITICAL_SECTION));
	if (!cs) return NULL;

	InitializeCriticalSection(cs);

	return cs;
}

void FAudio_PlatformLockMutex(FAudioMutex mutex)
{
	if (mutex) EnterCriticalSection(mutex);
}

void FAudio_PlatformUnlockMutex(FAudioMutex mutex)
{
	if (mutex) LeaveCriticalSection(mutex);
}

void FAudio_PlatformDestroyMutex(FAudioMutex mutex)
{
	if (mutex) DeleteCriticalSection(mutex);
	FAudio_free(mutex);
}

struct FAudioThreadArgs
{
	FAudioThreadFunc func;
	const char *name;
	void* data;
};

static DWORD WINAPI FAudioThreadWrapper(void *user)
{
	struct FAudioThreadArgs *args = user;
	DWORD ret;

	/* No thread naming: upstream names threads via SetThreadDescription
	 * resolved with LoadLibrary + GetProcAddress (desktop-only idiom),
	 * and the name is debugger-cosmetic only. */
	(void) args->name;
	ret = args->func(args->data);

	FAudio_free(args);
	return ret;
}

FAudioThread FAudio_PlatformCreateThread(
	FAudioThreadFunc func,
	const char *name,
	void* data
) {
	struct FAudioThreadArgs *args;

	if (!(args = FAudio_malloc(sizeof(*args)))) return NULL;
	args->func = func;
	args->name = name;
	args->data = data;

	return CreateThread(NULL, 0, &FAudioThreadWrapper, args, 0, NULL);
}

void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
	WaitForSingleObject(thread, INFINITE);
	if (retval != NULL) GetExitCodeThread(thread, (DWORD *)retval);
}

void FAudio_PlatformThreadPriority(FAudioThreadPriority priority)
{
	/* FIXME: upstream's win32 backend is a no-op here too. */
	(void) priority;
}

uint64_t FAudio_PlatformGetThreadID(void)
{
	return GetCurrentThreadId();
}

void FAudio_sleep(uint32_t ms)
{
	Sleep(ms);
}

uint32_t FAudio_timems()
{
	return GetTickCount();
}

/* lstrcpynA is declared only under WINAPI_PARTITION_DESKTOP | SYSTEM
 * (winbase.h), but FACT.c uses it via FAudio_strlcpy. Real implementation
 * with the documented semantics: copies at most iMaxLength-1 chars,
 * always NUL-terminates (when iMaxLength > 0), returns lpString1.
 * Declared in faudio_uwp_compat.h (force-included by the UWP build). */
LPSTR WINAPI lstrcpynA(LPSTR lpString1, LPCSTR lpString2, int iMaxLength)
{
	int i;
	if (lpString1 == NULL || iMaxLength <= 0)
	{
		return lpString1;
	}
	if (lpString2 == NULL)
	{
		lpString1[0] = '\0';
		return lpString1;
	}
	for (i = 0; i < iMaxLength - 1 && lpString2[i] != '\0'; i += 1)
	{
		lpString1[i] = lpString2[i];
	}
	lpString1[i] = '\0';
	return lpString1;
}

/* FAudio I/O (adapted from upstream's win32 backend: plain stdio, which
 * the UWP CRT provides). Used by FACT.c for XACT wavebanks. */

static size_t FAUDIOCALL FAudio_FILE_read(
	void *data,
	void *dst,
	size_t size,
	size_t count
) {
	if (!data) return 0;
	return fread(dst, size, count, data);
}

static int64_t FAUDIOCALL FAudio_FILE_seek(
	void *data,
	int64_t offset,
	int whence
) {
	if (!data) return -1;
	fseek(data, (long)offset, whence);
	return ftell(data);
}

static int FAUDIOCALL FAudio_FILE_close(void *data)
{
	if (!data) return 0;
	fclose(data);
	return 0;
}

FAudioIOStream* FAudio_fopen(const char *path)
{
	FAudioIOStream *io;
	errno_t err = -1;

	io = (FAudioIOStream*) FAudio_malloc(sizeof(FAudioIOStream));
	if (!io) return NULL;

	err = fopen_s((FILE**)&(io->data), path, "rb");

	if (err != 0 || io->data == NULL)
	{
		FAudio_free(io);
		return NULL;
	}

	io->read = FAudio_FILE_read;
	io->seek = FAudio_FILE_seek;
	io->close = FAudio_FILE_close;
	io->lock = FAudio_PlatformCreateMutex();

	return io;
}

struct FAudio_mem
{
	char *mem;
	int64_t len;
	int64_t pos;
};

static size_t FAUDIOCALL FAudio_mem_read(
	void *data,
	void *dst,
	size_t size,
	size_t count
) {
	struct FAudio_mem *io = data;
	size_t len = size * count;

	if (!data) return 0;

	while (len && len > (UINT)(io->len - io->pos)) len -= size;
	FAudio_memcpy(dst, io->mem + io->pos, len);
	io->pos += len;

	return len;
}

static int64_t FAUDIOCALL FAudio_mem_seek(
	void *data,
	int64_t offset,
	int whence
) {
	struct FAudio_mem *io = data;
	if (!data) return -1;

	if (whence == SEEK_SET)
	{
		if (io->len > offset) io->pos = offset;
		else io->pos = io->len;
	}
	if (whence == SEEK_CUR)
	{
		if (io->len > io->pos + offset) io->pos += offset;
		else io->pos = io->len;
	}
	if (whence == SEEK_END)
	{
		if (io->len > offset) io->pos = io->len - offset;
		else io->pos = 0;
	}

	return io->pos;
}

static int FAUDIOCALL FAudio_mem_close(void *data)
{
	if (!data) return 0;
	FAudio_free(data);
	return 0;
}

FAudioIOStream* FAudio_memopen(void *mem, int len)
{
	struct FAudio_mem *data;
	FAudioIOStream *io;

	io = (FAudioIOStream*) FAudio_malloc(sizeof(FAudioIOStream));
	if (!io) return NULL;

	data = FAudio_malloc(sizeof(struct FAudio_mem));
	if (!data)
	{
		FAudio_free(io);
		return NULL;
	}

	data->mem = mem;
	data->len = len;
	data->pos = 0;

	io->data = data;
	io->read = FAudio_mem_read;
	io->seek = FAudio_mem_seek;
	io->close = FAudio_mem_close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

uint8_t* FAudio_memptr(FAudioIOStream *io, size_t offset)
{
	struct FAudio_mem *memio = io->data;
	return (uint8_t *)(memio->mem + offset);
}

void FAudio_close(FAudioIOStream *io)
{
	io->close(io->data);
	FAudio_PlatformDestroyMutex((FAudioMutex) io->lock);
	FAudio_free(io);
}

/* XNA Song implementation over Media Foundation (adapted from upstream's
 * win32 backend). MFStartup / MFCreateSourceReaderFromURL /
 * MFCreateMediaType / MFCreateAttributes are APP-partition APIs. The
 * duration read avoids propvarutil.h (DESKTOP-only): MF_PD_DURATION is
 * always VT_UI8 (100ns units), read straight from the PROPVARIANT union;
 * VT_UI8 carries no allocation, so no PropVariantClear is needed. */

static FAudioWaveFormatEx activeSongFormat;
static IMFSourceReader *activeSong;
static uint8_t *songBuffer;
static SIZE_T songBufferSize;

static float songVolume = 1.0f;
static FAudio *songAudio = NULL;
static FAudioMasteringVoice *songMaster = NULL;

static FAudioSourceVoice *songVoice = NULL;
static FAudioVoiceCallback callbacks;

/* Internal Functions */

static void XNA_SongSubmitBuffer(FAudioVoiceCallback *callback, void *pBufferContext)
{
	IMFMediaBuffer *media_buffer;
	FAudioBuffer buffer;
	IMFSample *sample;
	HRESULT hr;
	DWORD flags, buffer_size = 0;
	BYTE *buffer_ptr;

	(void) callback;
	(void) pBufferContext;

	LOG_FUNC_ENTER(songAudio);

	FAudio_memset(&buffer, 0, sizeof(buffer));

	hr = IMFSourceReader_ReadSample(
		activeSong,
		MF_SOURCE_READER_FIRST_AUDIO_STREAM,
		0,
		NULL,
		&flags,
		NULL,
		&sample
	);
	FAudio_assert(!FAILED(hr) && "Failed to read audio sample!");

	if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
	{
		buffer.Flags = FAUDIO_END_OF_STREAM;
	}
	else
	{
		hr = IMFSample_ConvertToContiguousBuffer(
			sample,
			&media_buffer
		);
		FAudio_assert(!FAILED(hr) && "Failed to get sample buffer!");

		hr = IMFMediaBuffer_Lock(
			media_buffer,
			&buffer_ptr,
			NULL,
			&buffer_size
		);
		FAudio_assert(!FAILED(hr) && "Failed to lock buffer bytes!");

		if (songBufferSize < buffer_size)
		{
			songBufferSize = buffer_size;
			songBuffer = FAudio_realloc(songBuffer, songBufferSize);
			FAudio_assert(songBuffer != NULL && "Failed to allocate song buffer!");
		}
		FAudio_memcpy(songBuffer, buffer_ptr, buffer_size);

		hr = IMFMediaBuffer_Unlock(media_buffer);
		FAudio_assert(!FAILED(hr) && "Failed to unlock buffer bytes!");

		IMFMediaBuffer_Release(media_buffer);
		IMFSample_Release(sample);
	}

	if (buffer_size > 0)
	{
		buffer.AudioBytes = buffer_size;
		buffer.pAudioData = songBuffer;
		buffer.PlayBegin = 0;
		buffer.PlayLength = buffer_size / activeSongFormat.nBlockAlign;
		buffer.LoopBegin = 0;
		buffer.LoopLength = 0;
		buffer.LoopCount = 0;
		buffer.pContext = NULL;
		FAudioSourceVoice_SubmitSourceBuffer(
			songVoice,
			&buffer,
			NULL
		);
	}

	/* hr is only read by FAudio_assert, which compiles out in Release. */
	(void) hr;
	LOG_FUNC_EXIT(songAudio);
}

static void XNA_SongKill()
{
	if (songVoice != NULL)
	{
		FAudioSourceVoice_Stop(songVoice, 0, 0);
		FAudioVoice_DestroyVoice(songVoice);
		songVoice = NULL;
	}
	if (activeSong)
	{
		IMFSourceReader_Release(activeSong);
		activeSong = NULL;
	}
	FAudio_free(songBuffer);
	songBuffer = NULL;
	songBufferSize = 0;
}

/* "Public" API */

FAUDIOAPI void XNA_SongInit()
{
	HRESULT hr;

	hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
	FAudio_assert(!FAILED(hr) && "Failed to initialize Media Foundation!");
	(void) hr; /* only read by FAudio_assert, which compiles out in Release */

	FAudioCreate(&songAudio, 0, FAUDIO_DEFAULT_PROCESSOR);
	FAudio_CreateMasteringVoice(
		songAudio,
		&songMaster,
		FAUDIO_DEFAULT_CHANNELS,
		FAUDIO_DEFAULT_SAMPLERATE,
		0,
		0,
		NULL
	);
}

FAUDIOAPI void XNA_SongQuit()
{
	XNA_SongKill();
	FAudioVoice_DestroyVoice(songMaster);
	FAudio_Release(songAudio);
        MFShutdown();
}

FAUDIOAPI float XNA_PlaySong(const char *name)
{
	IMFAttributes *attributes = NULL;
	IMFMediaType *media_type = NULL;
	UINT32 channels, samplerate;
	UINT64 duration = 0;
	PROPVARIANT var;
	HRESULT hr;
	WCHAR filename_w[MAX_PATH];

	LOG_FUNC_ENTER(songAudio);
	LOG_INFO(songAudio, "name %s\n", name);
	XNA_SongKill();

	MultiByteToWideChar(CP_UTF8, 0, name, -1, filename_w, MAX_PATH);

	hr = MFCreateAttributes(&attributes, 1);
	FAudio_assert(!FAILED(hr) && "Failed to create attributes!");
	hr = MFCreateSourceReaderFromURL(
		filename_w,
		attributes,
		&activeSong
	);
	FAudio_assert(!FAILED(hr) && "Failed to create source reader!");
	IMFAttributes_Release(attributes);

	hr = MFCreateMediaType(&media_type);
	FAudio_assert(!FAILED(hr) && "Failed to create media type!");
	hr = IMFMediaType_SetGUID(
		media_type,
		&MF_MT_MAJOR_TYPE,
		&MFMediaType_Audio
	);
	FAudio_assert(!FAILED(hr) && "Failed to set major type!");
	hr = IMFMediaType_SetGUID(
		media_type,
		&MF_MT_SUBTYPE,
		&MFAudioFormat_Float
	);
	FAudio_assert(!FAILED(hr) && "Failed to set sub type!");
	hr = IMFSourceReader_SetCurrentMediaType(
		activeSong,
		MF_SOURCE_READER_FIRST_AUDIO_STREAM,
		NULL,
		media_type
	);
	FAudio_assert(!FAILED(hr) && "Failed to set source media type!");
	hr = IMFSourceReader_SetStreamSelection(
		activeSong,
		MF_SOURCE_READER_FIRST_AUDIO_STREAM,
		TRUE
	);
	FAudio_assert(!FAILED(hr) && "Failed to select source stream!");
	IMFMediaType_Release(media_type);

	hr = IMFSourceReader_GetCurrentMediaType(
		activeSong,
		MF_SOURCE_READER_FIRST_AUDIO_STREAM,
		&media_type
	);
	FAudio_assert(!FAILED(hr) && "Failed to get current media type!");
	hr = IMFMediaType_GetUINT32(
		media_type,
		&MF_MT_AUDIO_NUM_CHANNELS,
		&channels
	);
	FAudio_assert(!FAILED(hr) && "Failed to get channel count!");
	hr = IMFMediaType_GetUINT32(
		media_type,
		&MF_MT_AUDIO_SAMPLES_PER_SECOND,
		&samplerate
	);
	FAudio_assert(!FAILED(hr) && "Failed to get sample rate!");
	IMFMediaType_Release(media_type);

	FAudio_memset(&var, 0, sizeof(var));
	hr = IMFSourceReader_GetPresentationAttribute(
		activeSong,
		MF_SOURCE_READER_MEDIASOURCE,
		&MF_PD_DURATION,
		&var
	);
	FAudio_assert(!FAILED(hr) && "Failed to get song duration!");
	/* MF_PD_DURATION is always VT_UI8 (100ns units). Read the union
	 * directly: PropVariantToInt64 lives in propvarutil.h, which is
	 * DESKTOP-only. VT_UI8 needs no PropVariantClear. */
	if (var.vt == VT_UI8)
	{
		duration = (UINT64) var.uhVal.QuadPart;
	}

	activeSongFormat.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
	activeSongFormat.nChannels = channels;
	activeSongFormat.nSamplesPerSec = samplerate;
	activeSongFormat.wBitsPerSample = sizeof(float) * 8;
	activeSongFormat.nBlockAlign = activeSongFormat.nChannels * activeSongFormat.wBitsPerSample / 8;
	activeSongFormat.nAvgBytesPerSec = activeSongFormat.nSamplesPerSec * activeSongFormat.nBlockAlign;
	activeSongFormat.cbSize = 0;

	/* Init voice */
	FAudio_zero(&callbacks, sizeof(FAudioVoiceCallback));
	callbacks.OnBufferEnd = XNA_SongSubmitBuffer;
	FAudio_CreateSourceVoice(
		songAudio,
		&songVoice,
		&activeSongFormat,
		0,
		1.0f, /* No pitch shifting here! */
		&callbacks,
		NULL,
		NULL
	);
	FAudioVoice_SetVolume(songVoice, songVolume, 0);
	XNA_SongSubmitBuffer(NULL, NULL);

	/* Finally. */
	FAudioSourceVoice_Start(songVoice, 0, 0);
	(void) hr; /* only read by FAudio_assert, which compiles out in Release */
	LOG_FUNC_EXIT(songAudio);
	return (float)(duration / 10000000.);
}

FAUDIOAPI void XNA_PauseSong()
{
	if (songVoice == NULL)
	{
		return;
	}
	FAudioSourceVoice_Stop(songVoice, 0, 0);
}

FAUDIOAPI void XNA_ResumeSong()
{
	if (songVoice == NULL)
	{
		return;
	}
	FAudioSourceVoice_Start(songVoice, 0, 0);
}

FAUDIOAPI void XNA_StopSong()
{
	XNA_SongKill();
}

FAUDIOAPI void XNA_SetSongVolume(float volume)
{
	songVolume = volume;
	if (songVoice != NULL)
	{
		FAudioVoice_SetVolume(songVoice, songVolume, 0);
	}
}

FAUDIOAPI uint32_t XNA_GetSongEnded()
{
	FAudioVoiceState state;
	if (songVoice == NULL || activeSong == NULL)
	{
		return 1;
	}
	FAudioSourceVoice_GetState(songVoice, &state, 0);
	return state.BuffersQueued == 0 && state.SamplesPlayed == 0;
}

FAUDIOAPI void XNA_EnableVisualization(uint32_t enable)
{
	/* TODO: Enable/Disable FAPO effect */
	(void) enable;
}

FAUDIOAPI uint32_t XNA_VisualizationEnabled()
{
	/* TODO: Query FAPO effect enabled */
	return 0;
}

FAUDIOAPI void XNA_GetSongVisualizationData(
	float *frequencies,
	float *samples,
	uint32_t count
) {
	/* TODO: Visualization FAPO that reads in Song samples, FFT analysis */
	(void) frequencies;
	(void) samples;
	(void) count;
}

#else

extern int this_tu_is_empty;

#endif /* FAUDIO_UWP_PLATFORM */
