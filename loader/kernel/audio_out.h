#pragma once
// Host audio output (WASAPI) behind libSceAudioOut ports; see audio_out.cpp.
#include <cstdint>

namespace gr::kernel {

void AudioOutOpen(int port);
void AudioOutClose(int port);
// format: SceAudioOutParamFormat (0 S16 mono, 1 S16 stereo, 2/6 S16 8ch, 3 float mono, 4 float stereo, 5/7 float 8ch)
void AudioOutPush(int port, const void* data, uint32_t frames, uint32_t format, uint32_t freq);
void AudioOutSetVolume(int port, float l, float r);
void AudioOutSetMuted(bool muted); // --mute (tests)

} // namespace gr::kernel
