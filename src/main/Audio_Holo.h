#pragma once

#include <stdint.h>

void Audio_HoloShutdown(void);
void Audio_HoloSetMusicVolume(int volume);
int Audio_HoloActive(void);
uint32_t Audio_HoloMixedBlocks(void);
uint32_t Audio_HoloActiveVoices(void);
int32_t Audio_HoloLastError(void);
