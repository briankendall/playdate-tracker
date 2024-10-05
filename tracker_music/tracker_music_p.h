#ifndef TRACKER_MUSIC_P_H
#define TRACKER_MUSIC_P_H

#include "tracker_music.h"

#define ROWS_PER_PATTERN 64
#define NOTE_AND_INST_FLAG 0x20
#define VOLUME_FLAG 0x40
#define EFFECT_FLAG 0x80

#define UNSET 0xFF
#define NOTE_OFF 0xFE
#define SYNTH_DATA_UNINITIALIZED UINT32_MAX

static inline PatternCell * patternAtIndex(TrackerMusic *music, int i)
{
    return &music->patterns[ROWS_PER_PATTERN * music->channelCount * i];
}

static inline PatternCell * patternCell(TrackerMusic *music, PatternCell *pattern, int row, int channel)
{
    return &pattern[row * music->channelCount + channel];
}

int createTrackerMusicAudioEntities(TrackerMusic *music);

#endif // TRACKER_MUSIC_P_H
