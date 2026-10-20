/*
 * mf_to_wav.c - the sound of a media file (e.g. DuckStation's audio
 * capture, .mp4 with AAC) as a 16-bit PCM WAV, decoded by Windows' own
 * Media Foundation.  Used by tests/compare_sound.py.
 *
 *     mf_to_wav <in.mp4> <out.wav>
 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>

static void put32(FILE *f, unsigned v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, unsigned short v) { fwrite(&v, 2, 1, f); }

int wmain(int argc, wchar_t **argv)
{
    IMFSourceReader *reader = NULL;
    IMFMediaType *pcm = NULL, *actual = NULL;
    UINT32 rate = 0, channels = 0;
    unsigned data_bytes = 0;

    if (argc < 3) {
        fwprintf(stderr, L"usage: mf_to_wav <in> <out.wav>\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, 0))
        || FAILED(MFCreateSourceReaderFromURL(argv[1], NULL, &reader))) {
        fwprintf(stderr, L"cannot open %ls\n", argv[1]);
        return 1;
    }
    IMFSourceReader_SetStreamSelection(reader, (DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(reader, (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    MFCreateMediaType(&pcm);
    IMFMediaType_SetGUID(pcm, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(pcm, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(pcm, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(reader, (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                   NULL, pcm))
        || FAILED(IMFSourceReader_GetCurrentMediaType(reader,
                                                      (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                      &actual))) {
        fwprintf(stderr, L"no audio that can be decoded\n");
        return 1;
    }
    IMFMediaType_GetUINT32(actual, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(actual, &MF_MT_AUDIO_NUM_CHANNELS, &channels);

    FILE *out = _wfopen(argv[2], L"wb");
    if (!out) {
        return 1;
    }
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, out);
    put32(out, 16);
    put16(out, 1);
    put16(out, (unsigned short) channels);
    put32(out, rate);
    put32(out, rate * channels * 2);
    put16(out, (unsigned short) (channels * 2));
    put16(out, 16);
    fwrite("data\0\0\0\0", 1, 8, out);

    for (;;) {
        DWORD flags = 0;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(reader, (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                              NULL, &flags, NULL, &sample))
            || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            break;
        }
        if (!sample) {
            continue;
        }
        IMFMediaBuffer *buffer = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer))) {
            BYTE *data = NULL;
            DWORD length = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &length))) {
                fwrite(data, 1, length, out);
                data_bytes += length;
                IMFMediaBuffer_Unlock(buffer);
            }
            IMFMediaBuffer_Release(buffer);
        }
        IMFSample_Release(sample);
    }
    fseek(out, 4, SEEK_SET);
    put32(out, 36 + data_bytes);
    fseek(out, 40, SEEK_SET);
    put32(out, data_bytes);
    fclose(out);
    printf("%u Hz, %u channels, %.1f s\n", rate, channels,
           (double) data_bytes / (rate * channels * 2));
    MFShutdown();
    return 0;
}
