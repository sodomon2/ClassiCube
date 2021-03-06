#include "Audio.h"
#include "Logger.h"
#include "Platform.h"
#include "Event.h"
#include "Block.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "GameStructs.h"
#include "Errors.h"
#include "Vorbis.h"
#include "Chat.h"
#include "Stream.h"
#include "Utils.h"

int Audio_SoundsVolume, Audio_MusicVolume;

/* TODO: Fix for android */
#if defined CC_BUILD_NOAUDIO
/* Can't use mojang's sounds or music assets, so just stub everything out */
static void Audio_Init(void) { }
static void Audio_Free(void) { }

void Audio_SetMusic(int volume) {
	if (volume) Chat_AddRaw("&cMusic is not supported currently");
}
void Audio_SetSounds(int volume) {
	if (volume) Chat_AddRaw("&cSounds are not supported currently");
}

void Audio_PlayDigSound(cc_uint8 type) { }
void Audio_PlayStepSound(cc_uint8 type) { }
#else
#if defined CC_BUILD_WINMM
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <mmsystem.h>
#elif defined CC_BUILD_OPENAL
#include <pthread.h>
#if defined CC_BUILD_OSX
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif
#endif
static StringsBuffer files;

static void Volume_Mix16(cc_int16* samples, int count, int volume) {
	int i;

	for (i = 0; i < (count & ~0x07); i += 8, samples += 8) {
		samples[0] = (samples[0] * volume / 100);
		samples[1] = (samples[1] * volume / 100);
		samples[2] = (samples[2] * volume / 100);
		samples[3] = (samples[3] * volume / 100);

		samples[4] = (samples[4] * volume / 100);
		samples[5] = (samples[5] * volume / 100);
		samples[6] = (samples[6] * volume / 100);
		samples[7] = (samples[7] * volume / 100);
	}

	for (; i < count; i++, samples++) {
		samples[0] = (samples[0] * volume / 100);
	}
}

static void Volume_Mix8(cc_uint8* samples, int count, int volume) {
	int i;
	for (i = 0; i < count; i++, samples++) {
		samples[0] = (127 + (samples[0] - 127) * volume / 100);
	}
}


/*########################################################################################################################*
*------------------------------------------------Native implementation----------------------------------------------------*
*#########################################################################################################################*/
static cc_result Audio_AllCompleted(AudioHandle handle, cc_bool* finished);
#if defined CC_BUILD_WINMM
struct AudioContext {
	HWAVEOUT Handle;
	WAVEHDR Headers[AUDIO_MAX_BUFFERS];
	struct AudioFormat Format;
	int Count;
};
static struct AudioContext Audio_Contexts[20];
static void Audio_SysInit(void) { }
static void Audio_SysFree(void) { }

void Audio_Open(AudioHandle* handle, int buffers) {
	struct AudioContext* ctx;
	int i, j;

	for (i = 0; i < Array_Elems(Audio_Contexts); i++) {
		ctx = &Audio_Contexts[i];
		if (ctx->Count) continue;

		for (j = 0; j < buffers; j++) {
			ctx->Headers[j].dwFlags = WHDR_DONE;
		}

		*handle    = i;
		ctx->Count = buffers;
		return;
	}
	Logger_Abort("No free audio contexts");
}

cc_result Audio_Close(AudioHandle handle) {
	struct AudioFormat fmt = { 0 };
	struct AudioContext* ctx;
	cc_result res;
	ctx = &Audio_Contexts[handle];

	ctx->Count  = 0;
	ctx->Format = fmt;
	if (!ctx->Handle) return 0;

	res = waveOutClose(ctx->Handle);
	ctx->Handle = NULL;
	return res;
}

cc_result Audio_SetFormat(AudioHandle handle, struct AudioFormat* format) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	struct AudioFormat*  cur = &ctx->Format;
	cc_result res;

	if (AudioFormat_Eq(cur, format)) return 0;
	if (ctx->Handle && (res = waveOutClose(ctx->Handle))) return res;

	int sampleSize = format->Channels * format->BitsPerSample / 8;
	WAVEFORMATEX fmt;
	fmt.wFormatTag      = WAVE_FORMAT_PCM;
	fmt.nChannels       = format->Channels;
	fmt.nSamplesPerSec  = format->SampleRate;
	fmt.nAvgBytesPerSec = format->SampleRate * sampleSize;
	fmt.nBlockAlign     = sampleSize;
	fmt.wBitsPerSample  = format->BitsPerSample;
	fmt.cbSize          = 0;

	ctx->Format = *format;
	return waveOutOpen(&ctx->Handle, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
}

cc_result Audio_BufferData(AudioHandle handle, int idx, void* data, cc_uint32 dataSize) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	WAVEHDR* hdr = &ctx->Headers[idx];
	cc_result res;

	Mem_Set(hdr, 0, sizeof(WAVEHDR));
	hdr->lpData         = (LPSTR)data;
	hdr->dwBufferLength = dataSize;
	hdr->dwLoops        = 1;
	
	if ((res = waveOutPrepareHeader(ctx->Handle, hdr, sizeof(WAVEHDR)))) return res;
	if ((res = waveOutWrite(ctx->Handle, hdr, sizeof(WAVEHDR))))         return res;
	return 0;
}

cc_result Audio_Play(AudioHandle handle) { return 0; }

cc_result Audio_Stop(AudioHandle handle) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	if (!ctx->Handle) return 0;
	return waveOutReset(ctx->Handle);
}

cc_result Audio_IsCompleted(AudioHandle handle, int idx, cc_bool* completed) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	WAVEHDR* hdr = &ctx->Headers[idx];

	*completed = false;
	if (!(hdr->dwFlags & WHDR_DONE)) return 0;
	cc_result res = 0;

	if (hdr->dwFlags & WHDR_PREPARED) {
		res = waveOutUnprepareHeader(ctx->Handle, hdr, sizeof(WAVEHDR));
	}
	*completed = true; return res;
}

cc_result Audio_IsFinished(AudioHandle handle, cc_bool* finished) { return Audio_AllCompleted(handle, finished); }
#elif defined CC_BUILD_OPENAL
struct AudioContext {
	ALuint Source;
	ALuint Buffers[AUDIO_MAX_BUFFERS];
	cc_bool Completed[AUDIO_MAX_BUFFERS];
	struct AudioFormat Format;
	int Count;
	ALenum DataFormat;
};
static struct AudioContext Audio_Contexts[20];

static pthread_mutex_t audio_lock;
static ALCdevice* audio_device;
static ALCcontext* audio_context;
static volatile int audio_refs;

static void Audio_SysInit(void) {
	pthread_mutex_init(&audio_lock, NULL);
}
static void Audio_SysFree(void) {
	pthread_mutex_destroy(&audio_lock);
}

static void Audio_CheckContextErrors(void) {
	ALenum err = alcGetError(audio_device);
	if (err) Logger_Abort2(err, "Error creating OpenAL context");
}

static void Audio_CreateContext(void) {
	audio_device = alcOpenDevice(NULL);
	if (!audio_device) Logger_Abort("Failed to create OpenAL device");
	Audio_CheckContextErrors();

	audio_context = alcCreateContext(audio_device, NULL);
	if (!audio_context) {
		alcCloseDevice(audio_device);
		Logger_Abort("Failed to create OpenAL context");
	}
	Audio_CheckContextErrors();

	alcMakeContextCurrent(audio_context);
	Audio_CheckContextErrors();
}

static void Audio_DestroyContext(void) {
	if (!audio_device) return;
	alcMakeContextCurrent(NULL);

	if (audio_context) alcDestroyContext(audio_context);
	if (audio_device)  alcCloseDevice(audio_device);

	audio_context = NULL;
	audio_device  = NULL;
}

static ALenum Audio_FreeSource(struct AudioContext* ctx) {
	ALenum err;
	if (ctx->Source == -1) return 0;

	alDeleteSources(1, &ctx->Source);
	ctx->Source = -1;
	if ((err = alGetError())) return err;

	alDeleteBuffers(ctx->Count, ctx->Buffers);
	if ((err = alGetError())) return err;
	return 0;
}

void Audio_Open(AudioHandle* handle, int buffers) {
	ALenum err;
	int i, j;

	Mutex_Lock(&audio_lock);
	{
		if (!audio_context) Audio_CreateContext();
		audio_refs++;
	}
	Mutex_Unlock(&audio_lock);

	alDistanceModel(AL_NONE);

	for (i = 0; i < Array_Elems(Audio_Contexts); i++) {
		struct AudioContext* ctx = &Audio_Contexts[i];
		if (ctx->Count) continue;

		for (j = 0; j < buffers; j++) {
			ctx->Completed[j] = true;
		}

		*handle     = i;
		ctx->Count  = buffers;
		ctx->Source = -1;
		return;
	}
	Logger_Abort("No free audio contexts");
}

cc_result Audio_Close(AudioHandle handle) {
	struct AudioFormat fmt = { 0 };
	struct AudioContext* ctx;
	ALenum err;
	ctx = &Audio_Contexts[handle];

	if (!ctx->Count) return 0;
	ctx->Count  = 0;
	ctx->Format = fmt;

	err = Audio_FreeSource(ctx);
	if (err) return err;

	Mutex_Lock(&audio_lock);
	{
		audio_refs--;
		if (audio_refs == 0) Audio_DestroyContext();
	}
	Mutex_Unlock(&audio_lock);
	return 0;
}

static ALenum GetALFormat(int channels, int bitsPerSample) {
	if (bitsPerSample == 16) {
		if (channels == 1) return AL_FORMAT_MONO16;
		if (channels == 2) return AL_FORMAT_STEREO16;
	} else if (bitsPerSample == 8) {
		if (channels == 1) return AL_FORMAT_MONO8;
		if (channels == 2) return AL_FORMAT_STEREO8;
	}
	Logger_Abort("Unsupported audio format"); return 0;
}

cc_result Audio_SetFormat(AudioHandle handle, struct AudioFormat* format) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	struct AudioFormat*  cur = &ctx->Format;
	ALenum err;

	if (AudioFormat_Eq(cur, format)) return 0;
	ctx->DataFormat = GetALFormat(format->Channels, format->BitsPerSample);
	ctx->Format     = *format;
	
	if ((err = Audio_FreeSource(ctx))) return err;
	alGenSources(1, &ctx->Source);
	if ((err = alGetError())) return err;

	alGenBuffers(ctx->Count, ctx->Buffers);
	if ((err = alGetError())) return err;
	return 0;
}

cc_result Audio_BufferData(AudioHandle handle, int idx, void* data, cc_uint32 dataSize) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	ALuint buffer = ctx->Buffers[idx];
	ALenum err;
	ctx->Completed[idx] = false;

	alBufferData(buffer, ctx->DataFormat, data, dataSize, ctx->Format.SampleRate);
	if ((err = alGetError())) return err;
	alSourceQueueBuffers(ctx->Source, 1, &buffer);
	if ((err = alGetError())) return err;
	return 0;
}

cc_result Audio_Play(AudioHandle handle) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	alSourcePlay(ctx->Source);
	return alGetError();
}

cc_result Audio_Stop(AudioHandle handle) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	alSourceStop(ctx->Source);
	return alGetError();
}

cc_result Audio_IsCompleted(AudioHandle handle, int idx, cc_bool* completed) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	ALint i, processed = 0;
	ALuint buffer;
	ALenum err;

	alGetSourcei(ctx->Source, AL_BUFFERS_PROCESSED, &processed);
	if ((err = alGetError())) return err;

	if (processed > 0) {
		alSourceUnqueueBuffers(ctx->Source, 1, &buffer);
		if ((err = alGetError())) return err;

		for (i = 0; i < ctx->Count; i++) {
			if (ctx->Buffers[i] == buffer) ctx->Completed[i] = true;
		}
	}
	*completed = ctx->Completed[idx]; return 0;
}

cc_result Audio_IsFinished(AudioHandle handle, cc_bool* finished) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	ALint state = 0;
	cc_result res;

	if (ctx->Source == -1) { *finished = true; return 0; }
	res = Audio_AllCompleted(handle, finished);
	if (res) return res;
	
	alGetSourcei(ctx->Source, AL_SOURCE_STATE, &state);
	*finished = state != AL_PLAYING; return 0;
}
#endif

static cc_result Audio_AllCompleted(AudioHandle handle, cc_bool* finished) {
	struct AudioContext* ctx = &Audio_Contexts[handle];
	cc_result res;
	int i;
	*finished = false;

	for (i = 0; i < ctx->Count; i++) {
		res = Audio_IsCompleted(handle, i, finished);
		if (res) return res;
		if (!(*finished)) return 0;
	}

	*finished = true;
	return 0;
}

struct AudioFormat* Audio_GetFormat(AudioHandle handle) {
	return &Audio_Contexts[handle].Format;
}

cc_result Audio_StopAndClose(AudioHandle handle) {
	cc_bool finished;
	Audio_Stop(handle);
	Audio_IsFinished(handle, &finished); /* unqueue buffers */
	return Audio_Close(handle);
}


/*########################################################################################################################*
*------------------------------------------------------Soundboard---------------------------------------------------------*
*#########################################################################################################################*/
struct Sound {
	struct AudioFormat Format;
	cc_uint8* Data; cc_uint32 Size;
};

#define AUDIO_MAX_SOUNDS 10
struct SoundGroup {
	String Name; int Count;
	struct Sound Sounds[AUDIO_MAX_SOUNDS];
};

struct Soundboard {
	RNGState Rnd; int Count;
	struct SoundGroup Groups[AUDIO_MAX_SOUNDS];
};

#define WAV_FourCC(a, b, c, d) (((cc_uint32)a << 24) | ((cc_uint32)b << 16) | ((cc_uint32)c << 8) | (cc_uint32)d)
#define WAV_FMT_SIZE 16

static cc_result Sound_ReadWaveData(struct Stream* stream, struct Sound* snd) {
	cc_uint32 fourCC, size;
	cc_uint8 tmp[WAV_FMT_SIZE];
	cc_result res;

	if ((res = Stream_Read(stream, tmp, 12))) return res;
	fourCC = Stream_GetU32_BE(&tmp[0]);
	if (fourCC != WAV_FourCC('R','I','F','F')) return WAV_ERR_STREAM_HDR;
	/* tmp[4] (4) file size */
	fourCC = Stream_GetU32_BE(&tmp[8]);
	if (fourCC != WAV_FourCC('W','A','V','E')) return WAV_ERR_STREAM_TYPE;

	for (;;) {
		if ((res = Stream_Read(stream, tmp, 8))) return res;
		fourCC = Stream_GetU32_BE(&tmp[0]);
		size   = Stream_GetU32_LE(&tmp[4]);

		if (fourCC == WAV_FourCC('f','m','t',' ')) {
			if ((res = Stream_Read(stream, tmp, sizeof(tmp)))) return res;
			if (Stream_GetU16_LE(&tmp[0]) != 1) return WAV_ERR_DATA_TYPE;

			snd->Format.Channels      = Stream_GetU16_LE(&tmp[2]);
			snd->Format.SampleRate    = Stream_GetU32_LE(&tmp[4]);
			/* tmp[8] (6) alignment data and stuff */
			snd->Format.BitsPerSample = Stream_GetU16_LE(&tmp[14]);
			size -= WAV_FMT_SIZE;
		} else if (fourCC == WAV_FourCC('d','a','t','a')) {
			snd->Data = (cc_uint8*)Mem_TryAlloc(size, 1);
			snd->Size = size;

			if (!snd->Data) return ERR_OUT_OF_MEMORY;
			return Stream_Read(stream, snd->Data, size);
		}

		/* Skip over unhandled data */
		if (size && (res = stream->Skip(stream, size))) return res;
	}
}

static cc_result Sound_ReadWave(const String* filename, struct Sound* snd) {
	String path; char pathBuffer[FILENAME_SIZE];
	struct Stream stream;
	cc_result res;

	String_InitArray(path, pathBuffer);
	String_Format1(&path, "audio/%s", filename);

	res = Stream_OpenFile(&stream, &path);
	if (res) return res;

	res = Sound_ReadWaveData(&stream, snd);
	if (res) { stream.Close(&stream); return res; }

	return stream.Close(&stream);
}

static struct SoundGroup* Soundboard_Find(struct Soundboard* board, const String* name) {
	struct SoundGroup* groups = board->Groups;
	int i;

	for (i = 0; i < board->Count; i++) {
		if (String_CaselessEquals(&groups[i].Name, name)) return &groups[i];
	}
	return NULL;
}

static void Soundboard_Init(struct Soundboard* board, const String* boardName, StringsBuffer* files) {
	String file, name;
	struct SoundGroup* group;
	struct Sound* snd;
	cc_result res;
	int i, dotIndex;

	for (i = 0; i < files->count; i++) {
		file = StringsBuffer_UNSAFE_Get(files, i); 
		name = file;

		/* dig_grass1.wav -> dig_grass1 */
		dotIndex = String_LastIndexOf(&name, '.');
		if (dotIndex >= 0) { name = String_UNSAFE_Substring(&name, 0, dotIndex); }
		if (!String_CaselessStarts(&name, boardName)) continue;

		/* Convert dig_grass1 to grass */
		name = String_UNSAFE_SubstringAt(&name, boardName->length);
		name = String_UNSAFE_Substring(&name, 0, name.length - 1);

		group = Soundboard_Find(board, &name);
		if (!group) {
			if (board->Count == Array_Elems(board->Groups)) {
				Chat_AddRaw("&cCannot have more than 10 sound groups"); return;
			}

			group = &board->Groups[board->Count++];
			/* NOTE: This keeps a reference to inside buffer of files */
			group->Name = name;
		}

		if (group->Count == Array_Elems(group->Sounds)) {
			Chat_AddRaw("&cCannot have more than 10 sounds in a group"); return;
		}

		snd = &group->Sounds[group->Count];
		res = Sound_ReadWave(&file, snd);

		if (res) {
			Logger_Warn2(res, "decoding", &file);
			Mem_Free(snd->Data);
			snd->Data = NULL;
			snd->Size = 0;
		} else { group->Count++; }
	}
}

static struct Sound* Soundboard_PickRandom(struct Soundboard* board, cc_uint8 type) {
	String name;
	struct SoundGroup* group;
	int idx;

	if (type == SOUND_NONE || type >= SOUND_COUNT) return NULL;
	if (type == SOUND_METAL) type = SOUND_STONE;

	name  = String_FromReadonly(Sound_Names[type]);
	group = Soundboard_Find(board, &name);
	if (!group) return NULL;

	idx = Random_Next(&board->Rnd, group->Count);
	return &group->Sounds[idx];
}


/*########################################################################################################################*
*--------------------------------------------------------Sounds-----------------------------------------------------------*
*#########################################################################################################################*/
struct SoundOutput { AudioHandle Handle; void* Buffer; cc_uint32 BufferSize; };
#define AUDIO_MAX_HANDLES 6
#define HANDLE_INV -1
#define SOUND_INV { HANDLE_INV, NULL, 0 }

static struct Soundboard digBoard, stepBoard;
static struct SoundOutput monoOutputs[AUDIO_MAX_HANDLES]   = { SOUND_INV, SOUND_INV, SOUND_INV, SOUND_INV, SOUND_INV, SOUND_INV };
static struct SoundOutput stereoOutputs[AUDIO_MAX_HANDLES] = { SOUND_INV, SOUND_INV, SOUND_INV, SOUND_INV, SOUND_INV, SOUND_INV };

CC_NOINLINE static void Sounds_Fail(cc_result res) {
	Logger_SimpleWarn(res, "playing sounds");
	Chat_AddRaw("&cDisabling sounds");
	Audio_SetSounds(0);
}

static void Sounds_PlayRaw(struct SoundOutput* output, struct Sound* snd, struct AudioFormat* fmt, int volume) {
	void* data = snd->Data;
	cc_result res;
	if ((res = Audio_SetFormat(output->Handle, fmt))) { Sounds_Fail(res); return; }
	
	/* copy to temp buffer to apply volume */
	if (volume < 100) {		
		if (output->BufferSize < snd->Size) {
			/* TODO: check if we can realloc NULL without a problem */
			if (output->Buffer) {
				output->Buffer = Mem_Realloc(output->Buffer, snd->Size, 1, "sound temp buffer");
			} else {
				output->Buffer = Mem_Alloc(snd->Size, 1, "sound temp buffer");
			}
		}
		data = output->Buffer;

		Mem_Copy(data, snd->Data, snd->Size);
		if (fmt->BitsPerSample == 8) {
			Volume_Mix8((cc_uint8*)data,  snd->Size,     volume);
		} else {
			Volume_Mix16((cc_int16*)data, snd->Size / 2, volume);
		}
	}

	if ((res = Audio_BufferData(output->Handle, 0, data, snd->Size))) { Sounds_Fail(res); return; }
	if ((res = Audio_Play(output->Handle)))                           { Sounds_Fail(res); return; }
}

static void Sounds_Play(cc_uint8 type, struct Soundboard* board) {
	struct Sound* snd;
	struct AudioFormat  fmt;
	struct SoundOutput* outputs;
	struct SoundOutput* output;
	struct AudioFormat* l;

	cc_bool finished;
	int i, volume;
	cc_result res;

	if (type == SOUND_NONE || !Audio_SoundsVolume) return;
	snd = Soundboard_PickRandom(board, type);
	if (!snd) return;

	fmt     = snd->Format;
	volume  = Audio_SoundsVolume;
	outputs = fmt.Channels == 1 ? monoOutputs : stereoOutputs;

	if (board == &digBoard) {
		if (type == SOUND_METAL) fmt.SampleRate = (fmt.SampleRate * 6) / 5;
		else fmt.SampleRate = (fmt.SampleRate * 4) / 5;
	} else {
		volume /= 2;
		if (type == SOUND_METAL) fmt.SampleRate = (fmt.SampleRate * 7) / 5;
	}

	/* Try to play on fresh device, or device with same data format */
	for (i = 0; i < AUDIO_MAX_HANDLES; i++) {
		output = &outputs[i];
		if (output->Handle == HANDLE_INV) {
			Audio_Open(&output->Handle, 1);
		} else {
			res = Audio_IsFinished(output->Handle, &finished);

			if (res) { Sounds_Fail(res); return; }
			if (!finished) continue;
		}

		l = Audio_GetFormat(output->Handle);
		if (!l->Channels || AudioFormat_Eq(l, &fmt)) {
			Sounds_PlayRaw(output, snd, &fmt, volume); return;
		}
	}

	/* Try again with all devices, even if need to recreate one (expensive) */
	for (i = 0; i < AUDIO_MAX_HANDLES; i++) {
		output = &outputs[i];
		res = Audio_IsFinished(output->Handle, &finished);

		if (res) { Sounds_Fail(res); return; }
		if (!finished) continue;

		Sounds_PlayRaw(output, snd, &fmt, volume); return;
	}
}

static void Audio_PlayBlockSound(void* obj, IVec3 coords, BlockID old, BlockID now) {
	if (now == BLOCK_AIR) {
		Audio_PlayDigSound(Blocks.DigSounds[old]);
	} else if (!Game_ClassicMode) {
		Audio_PlayDigSound(Blocks.StepSounds[now]);
	}
}

static void Sounds_FreeOutputs(struct SoundOutput* outputs) {
	int i;
	for (i = 0; i < AUDIO_MAX_HANDLES; i++) {
		if (outputs[i].Handle == HANDLE_INV) continue;

		Audio_StopAndClose(outputs[i].Handle);
		outputs[i].Handle = HANDLE_INV;

		Mem_Free(outputs[i].Buffer);
		outputs[i].Buffer     = NULL;
		outputs[i].BufferSize = 0;
	}
}

static void Sounds_Init(void) {
	static const String dig  = String_FromConst("dig_");
	static const String step = String_FromConst("step_");

	if (digBoard.Count || stepBoard.Count) return;
	Soundboard_Init(&digBoard,  &dig,  &files);
	Soundboard_Init(&stepBoard, &step, &files);
}

static void Sounds_Free(void) {
	Sounds_FreeOutputs(monoOutputs);
	Sounds_FreeOutputs(stereoOutputs);
}

void Audio_SetSounds(int volume) {
	if (volume) Sounds_Init();
	else        Sounds_Free();
	Audio_SoundsVolume = volume;
}

void Audio_PlayDigSound(cc_uint8 type)  { Sounds_Play(type, &digBoard); }
void Audio_PlayStepSound(cc_uint8 type) { Sounds_Play(type, &stepBoard); }


/*########################################################################################################################*
*--------------------------------------------------------Music------------------------------------------------------------*
*#########################################################################################################################*/
static AudioHandle music_out;
static void* music_thread;
static void* music_waitable;
static volatile cc_bool music_pendingStop, music_joining;

static cc_result Music_Buffer(int i, cc_int16* data, int maxSamples, struct VorbisState* ctx) {
	int samples = 0;
	cc_int16* cur;
	cc_result res = 0, res2;

	while (samples < maxSamples) {
		if ((res = Vorbis_DecodeFrame(ctx))) break;

		cur = &data[samples];
		samples += Vorbis_OutputFrame(ctx, cur);
	}
	if (Audio_MusicVolume < 100) { Volume_Mix16(data, samples, Audio_MusicVolume); }

	res2 = Audio_BufferData(music_out, i, data, samples * 2);
	if (res2) { music_pendingStop = true; return res2; }
	return res;
}

static cc_result Music_PlayOgg(struct Stream* source) {
	cc_uint8 buffer[OGG_BUFFER_SIZE];
	struct Stream stream;
	struct VorbisState vorbis = { 0 };
	struct AudioFormat fmt;

	int chunkSize, samplesPerSecond;
	cc_int16* data = NULL;
	cc_bool completed;
	int i, next;
	cc_result res;

	Ogg_MakeStream(&stream, buffer, source);
	vorbis.source = &stream;
	if ((res = Vorbis_DecodeHeaders(&vorbis))) goto cleanup;
	
	fmt.Channels      = vorbis.channels;
	fmt.SampleRate    = vorbis.sampleRate;
	fmt.BitsPerSample = 16;
	if ((res = Audio_SetFormat(music_out, &fmt))) goto cleanup;

	/* largest possible vorbis frame decodes to blocksize1 * channels samples */
	/* so we may end up decoding slightly over a second of audio */
	chunkSize        = fmt.Channels * (fmt.SampleRate + vorbis.blockSizes[1]);
	samplesPerSecond = fmt.Channels * fmt.SampleRate;
	data = (cc_int16*)Mem_Alloc(chunkSize * AUDIO_MAX_BUFFERS, 2, "Ogg final output");

	/* fill up with some samples before playing */
	for (i = 0; i < AUDIO_MAX_BUFFERS && !res; i++) {
		res = Music_Buffer(i, &data[chunkSize * i], samplesPerSecond, &vorbis);
	}
	if (music_pendingStop) goto cleanup;

	res = Audio_Play(music_out);
	if (res) goto cleanup;

	for (;;) {
		next = -1;
		
		for (i = 0; i < AUDIO_MAX_BUFFERS; i++) {
			res = Audio_IsCompleted(music_out, i, &completed);
			if (res)       { music_pendingStop = true; break; }
			if (completed) { next = i; break; }
		}

		if (next == -1) { Thread_Sleep(10); continue; }
		if (music_pendingStop) break;

		res = Music_Buffer(next, &data[chunkSize * next], samplesPerSecond, &vorbis);
		/* need to specially handle last bit of audio */
		if (res) break;
	}

	if (music_pendingStop) Audio_Stop(music_out);
	/* Wait until the buffers finished playing */
	for (;;) {
		if (Audio_IsFinished(music_out, &completed) || completed) break;
		Thread_Sleep(10);
	}

cleanup:
	Mem_Free(data);
	Vorbis_Free(&vorbis);
	return res == ERR_END_OF_STREAM ? 0 : res;
}

#define MUSIC_MAX_FILES 512
static void Music_RunLoop(void) {
	static const String ogg = String_FromConst(".ogg");
	char pathBuffer[FILENAME_SIZE];
	String path;

	unsigned short musicFiles[MUSIC_MAX_FILES];
	String file;

	RNGState rnd;
	struct Stream stream;
	int i, count = 0, idx, delay;
	cc_result res = 0;

	for (i = 0; i < files.count && count < MUSIC_MAX_FILES; i++) {
		file = StringsBuffer_UNSAFE_Get(&files, i);
		if (!String_CaselessEnds(&file, &ogg)) continue;
		musicFiles[count++] = i;
	}

	Random_SeedFromCurrentTime(&rnd);
	Audio_Open(&music_out, AUDIO_MAX_BUFFERS);

	while (!music_pendingStop && count) {
		idx  = Random_Next(&rnd, count);
		file = StringsBuffer_UNSAFE_Get(&files, musicFiles[idx]);
		
		String_InitArray(path, pathBuffer);
		String_Format1(&path, "audio/%s", &file);
		Platform_Log1("playing music file: %s", &file);

		res = Stream_OpenFile(&stream, &path);
		if (res) { Logger_Warn2(res, "opening", &path); break; }

		res = Music_PlayOgg(&stream);
		if (res) { 
			Logger_SimpleWarn2(res, "playing", &path); 
			stream.Close(&stream); break;
		}

		res = stream.Close(&stream);
		if (res) { Logger_Warn2(res, "closing", &path); break; }

		if (music_pendingStop) break;
		delay = 120 * 1000 + Random_Next(&rnd, 300 * 1000);
		Waitable_WaitFor(music_waitable, delay);
	}

	if (res) {
		Chat_AddRaw("&cDisabling music");
		Audio_MusicVolume = 0;
	}
	Audio_StopAndClose(music_out);

	if (music_joining) return;
	Thread_Detach(music_thread);
	music_thread = NULL;
}

static void Music_Init(void) {
	if (music_thread) return;
	music_joining     = false;
	music_pendingStop = false;

	music_thread = Thread_Start(Music_RunLoop, false);
}

static void Music_Free(void) {
	music_joining     = true;
	music_pendingStop = true;
	Waitable_Signal(music_waitable);
	
	if (music_thread) Thread_Join(music_thread);
	music_thread = NULL;
}

void Audio_SetMusic(int volume) {
	if (volume) Music_Init();
	else        Music_Free();
	Audio_MusicVolume = volume;
}


/*########################################################################################################################*
*--------------------------------------------------------General----------------------------------------------------------*
*#########################################################################################################################*/
static int Audio_LoadVolume(const char* volKey, const char* boolKey) {
	int volume = Options_GetInt(volKey, 0, 100, 0);
	if (volume) return volume;

	volume = Options_GetBool(boolKey, false) ? 100 : 0;
	Options_Set(boolKey, NULL);
	return volume;
}

static void Audio_FilesCallback(const String* path, void* obj) {
	String relPath = *path; 
	Utils_UNSAFE_TrimFirstDirectory(&relPath);
	StringsBuffer_Add(&files, &relPath);
}

static void Audio_Init(void) {
	static const String path = String_FromConst("audio");
	int volume;

	Directory_Enum(&path, NULL, Audio_FilesCallback);
	music_waitable = Waitable_Create();
	Audio_SysInit();

	volume = Audio_LoadVolume(OPT_MUSIC_VOLUME, OPT_USE_MUSIC);
	Audio_SetMusic(volume);
	volume = Audio_LoadVolume(OPT_SOUND_VOLUME, OPT_USE_SOUND);
	Audio_SetSounds(volume);
	Event_RegisterBlock(&UserEvents.BlockChanged, NULL, Audio_PlayBlockSound);
}

static void Audio_Free(void) {
	Music_Free();
	Sounds_Free();
	Waitable_Free(music_waitable);
	Audio_SysFree();
	Event_UnregisterBlock(&UserEvents.BlockChanged, NULL, Audio_PlayBlockSound);
}
#endif

struct IGameComponent Audio_Component = {
	Audio_Init, /* Init  */
	Audio_Free  /* Free  */
};
