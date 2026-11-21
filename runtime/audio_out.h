// The host audio output of audout:u (ov_audout.c): one thread that asks for float frames at 48 kHz and hands them to the
// device. audio_out_win.c is WASAPI in shared mode (Windows), audio_out_sdl.c is an SDL3 stream (other hosts); the build
// picks one by its suffix.
//
// The game mixes 2 or 6 channels (FL FR FC LFE SL SR, the order of Wwise and of WAVE_FORMAT_EXTENSIBLE mask 0x60f).
// The output asks the device for that many; when the device takes no 5.1 stream it asks for stereo and the fill folds
// the 5.1 down itself. Without any device the thread still asks for frames at the rate of the clock and throws them
// away, so the game's audio thread, paced by the buffers coming back, and the device clock frame_rate.c reads go on.
#pragma once

// Fills frames frames of channels interleaved floats: the stream's channels, 2 for a fold-down of a 5.1 mix.
typedef void (*AudioOutFill)(float *dst, int frames, int channels);

typedef struct {
    int open;              // 1: a device plays the stream; 0: none (the null output runs, or nothing started yet)
    int started;           // audio_out_start ran: the fields below describe its last device
    char name[128];        // the device's name, UTF-8
    int device_channels;   // the channels the device's mixer runs at (WASAPI: the shared-mode mix format)
    int stream_channels;   // what the stream carries: the game's count, or 2 when a 5.1 mix is folded down here
    int spatial;           // Windows spatial sound (Windows Sonic, Dolby Atmos, DTS) on the device: 1, 0; -1 unknown
    int surround_ok;       // the device takes a 5.1 stream as it is (its own channels, or a spatial sound renderer)
} AudioOutInfo;

// Starts the output thread for a game that mixes channels (2 or 6) and returns once its first device has been tried, so
// the log and audio_out_info describe it. 1 when the thread runs (with a device or the null output), 0 when it could
// not start: then the caller treats every buffer as played at once.
int audio_out_start(int channels, AudioOutFill fill);
// The output's last state, any thread.
void audio_out_info(AudioOutInfo *out);
