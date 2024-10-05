#ifndef S3M_H
#define S3M_H

#include <stdint.h>

#include "pd_api.h"

typedef struct _TrackerMusic TrackerMusic;

#define S3M_MAX_CHANNELS 32
#define S3M_TITLE_LENGTH 28
#define S3M_FILENAME_LENGTH 12
#define S3M_HEADER_MAGIC_1 0x1A
#define S3M_HEADER_MAGIC_2 "SCRM"

#define S3M_LOOPING_FLAG 0x01
#define S3M_STEREO_FLAG 0x02
#define S3M_16_BIT_FLAG 0x04

typedef struct _S3MHeader {
    char title[S3M_TITLE_LENGTH];
    uint8_t magicNumber1;
    uint8_t type; // unused
    uint8_t unused1[2];
    uint16_t orderCount;
    uint16_t instrumentCount;
    uint16_t patternCount;
    uint16_t oldFlags; // unused
    uint16_t trackerVersion; // unused
    uint16_t formatVersion; // unused
    char magicNumber2[4];
    uint8_t globalVolume;
    uint8_t initialSpeed;
    uint8_t initialTempo;
    uint8_t masterVolume; // unused
    uint8_t unused2;
    uint8_t defaultPan;
    uint8_t unused3[8];
    uint16_t ptrSpecial; // unused
    uint8_t channelSettings[32];
} __attribute__((packed)) S3MHeader;

typedef struct _S3MInstrument {
    uint8_t type;
    char name[S3M_FILENAME_LENGTH];
    uint8_t dataPtrHi;
    uint16_t dataPtrLo;
    uint32_t length;
    uint32_t loopBegin;
    uint32_t loopEnd;
    uint8_t volume;
    uint8_t unused;
    uint8_t pack;
    uint8_t flags;
    uint32_t c4Rate;
    uint8_t unused2[12];
    char title[S3M_TITLE_LENGTH];
    char magic[4];
} __attribute__((packed)) S3MInstrument;

_Static_assert (sizeof(S3MHeader) == 96, "S3M header struct is wrong size");
_Static_assert (sizeof(S3MInstrument) == 80, "S3M instrument struct is wrong size");

void initializeS3M(PlaydateAPI *inAPI);
int loadMusicFromS3M(TrackerMusic *music, char *path, FileOptions mode);

#endif
