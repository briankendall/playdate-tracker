#include "s3m.h"

#include "tracker_music.h"
#include "tracker_music_p.h"

#define printLog pd->system->logToConsole
#if TRACKER_MUSIC_VERBOSE
#define printLogVerbose pd->system->logToConsole
#else
#define printLogVerbose(...)
#endif
static PlaydateAPI *pd = NULL;

void initializeS3M(PlaydateAPI *inAPI)
{
    pd = inAPI;
}

static inline void * parapointerToPointer(uint8_t *rawData, uint32_t parapointer)
{
    return rawData + parapointer * 16;
}

static uint8_t s3mChannelPanFromData(uint8_t data)
{
    uint8_t val = data & 0x0F;
    
    if (data <= 8) {
        return val * 4;
    } else {
        return (val+1) * 4;
    }
}

static uint8_t s3mNoteToPDNote(uint8_t note)
{
    uint8_t octave = note >> 4;
    uint8_t pitch = note & 0x0F;
    return octave * 12 + pitch + 12;
}

static inline bool s3mIsNoteOff(uint8_t note) {
    return ((note >> 4) == 0xF) && note != 0xFF;
}

#define S3M_EFFECT_NUM(x) (x - 'A' + 1)

static uint8_t s3mEffectToEnum(uint8_t effect, uint8_t *effectVal, int patternIndex, int row)
{
    switch(effect) {
        case 0: // appears in some poorly formed s3m files
            return kEffectNone;
        case S3M_EFFECT_NUM('A'):
            return kEffectSetSpeed;
        case S3M_EFFECT_NUM('B'):
            return kEffectPositionJump;
        case S3M_EFFECT_NUM('C'):
            return kEffectPatternBreak;
        case S3M_EFFECT_NUM('D'):
            return kEffectVolumeSlide;
        case S3M_EFFECT_NUM('E'): 
            return kEffectPortamentoDown;
        case S3M_EFFECT_NUM('F'):
            return kEffectPortamentoUp;
        case S3M_EFFECT_NUM('G'):
            return kEffectTonePortamento;
        case S3M_EFFECT_NUM('H'):
            return kEffectVibrato;
        case S3M_EFFECT_NUM('I'):
            return kEffectTremor;
        case S3M_EFFECT_NUM('J'): 
            return kEffectArpeggio;
        case S3M_EFFECT_NUM('K'):
            return kEffectVolumeSlideAndVibrato;
        case S3M_EFFECT_NUM('L'):
            return kEffectVolumeSlideAndTonePortamento;
        case S3M_EFFECT_NUM('O'):
            return kEffectOffset;
        case S3M_EFFECT_NUM('P'):
            return kEffectPanningSlide;
        case S3M_EFFECT_NUM('Q'):
            return kEffectRetrigger;
        case S3M_EFFECT_NUM('R'):
            return kEffectTremolo;
        case S3M_EFFECT_NUM('S'): {
            uint8_t hi = (*effectVal & 0xF0) >> 4;
            (*effectVal) = (*effectVal) & 0x0F;
            
            switch (hi) {
                case 0x3:
                    return kEffectVibratoSetWaveform;
                case 0x4:
                    return kEffectTremoloSetWaveform;
                case 0x8:
                    return kEffectSetPanning;
                case 0xD:
                    return kEffectNoteDelay;
                default:
                    printLog("Warning: s3m file contains unimplemented effect: S%X at pattern %d row %d", hi,
                             patternIndex, row);
                    return kEffectNone;
            }
        }
        case S3M_EFFECT_NUM('T'):
            return kEffectSetTempo;
        case S3M_EFFECT_NUM('U'):
            return kEffectVibratoFine;
        case S3M_EFFECT_NUM('V'):
            return kEffectSetGlobalVolume;
        case S3M_EFFECT_NUM('X'):
            return kEffectSetPanningFine;
        default:
            printLog("Warning: s3m file contains unimplemented effect: %c (0x%02X) at pattern %d row %d",
                     (effect - 1 + 'A'), effect, patternIndex, row);
            return kEffectNone;
    }   
}

static int s3mReadChannels(TrackerMusic *music, S3MHeader *header)
{
    uint8_t *channelPan = music->rawData + sizeof(S3MHeader) + header->orderCount + header->instrumentCount * 2
                          + header->patternCount * 2;
    
    for(int i = 0; i < S3M_MAX_CHANNELS; ++i) {
        if (header->channelSettings[i] == 255 || (header->channelSettings[i] & 0x80) != 0) {
            continue;
        }
        
        if (i >= TRACKER_MUSIC_MAX_CHANNELS) {
            printLog("Error: s3m file has more channels than maximum! (%d)", TRACKER_MUSIC_MAX_CHANNELS);
            return kMusicTooManyChannelsError;
        }
        
        music->channelCount = i+1;
        music->channels[i].enabled = true;
        music->channels[i].pan = (header->defaultPan == 252) ? s3mChannelPanFromData(channelPan[i]) : 0x20;
    }
    
    return kMusicNoError;
}

static void s3mReadPattern(TrackerMusic *music, PatternCell *pattern, uint8_t *data, int patternIndex)
{
    uint8_t row = 0;
    uint16_t length = *((uint16_t *)data);
    uint8_t *end = data + length;
    data += 2;
    
    while(row < ROWS_PER_PATTERN && data < end) {
        PatternCell cell = {0};
        cell.what = *(data++);

        if (cell.what == 0) {
            ++row;
            continue;
        }

        uint8_t channel = cell.what & (S3M_MAX_CHANNELS - 1);

        if (cell.what & NOTE_AND_INST_FLAG) {
            cell.note = *(data++);
            cell.instrument = *(data++);
            
            if (cell.note == 255) {
                cell.note = 0;
            } else if (s3mIsNoteOff(cell.note)) {
                cell.note = NOTE_OFF;
            } else {
                cell.note = s3mNoteToPDNote(cell.note);
            }
        }

        if (cell.what & VOLUME_FLAG) {
            cell.volume = *(data++);
        }

        if (cell.what & EFFECT_FLAG) {
            cell.effect = *(data++);
            cell.effectVal = *(data++);
            cell.effect = s3mEffectToEnum(cell.effect, &cell.effectVal, patternIndex, row);
            
            if (cell.effect == kEffectNone) {
                cell.what = cell.what & (~EFFECT_FLAG);
            }
        }
        
        if (channel >= music->channelCount) {
            continue;
        }
        
        *patternCell(music, pattern, row, channel) = cell;
    }
}

static int s3mReadPatterns(TrackerMusic *music, S3MHeader *header)
{
    uint16_t *patternParapointers =
        (uint16_t *)(music->rawData + sizeof(S3MHeader) + header->orderCount + header->instrumentCount * 2);

    music->patternCount = header->patternCount;
    music->patterns =
        calloc(header->patternCount * music->channelCount * ROWS_PER_PATTERN, sizeof(PatternCell));

    if (!music->patterns) {
        printLog("Error: couldn't allocate memory for patterns!");
        return kMusicMemoryError;
    }
    
    for (uint16_t i = 0; i < header->patternCount; ++i) {
        if (patternParapointers[i] == 0) {
            continue;
        }

        s3mReadPattern(music, patternAtIndex(music, i), music->rawData + patternParapointers[i] * 16, i);
    }
    
    for(int orderIndex = 0; orderIndex < header->orderCount; ++orderIndex) {
        int patternIndex = music->orders[orderIndex];
        
        if (patternIndex == 0xFE) {
            printLog("Error: s3m marker patterns are not supported");
            return kMusicUnsupportedS3MError;
        }
        
        if (patternIndex == 0xFF || patternIndex < 0 || patternIndex >= music->patternCount) {
            break;
        }
        
        ++music->orderCount;
    }
    
    return kMusicNoError;
}

static int s3mReadInstruments(TrackerMusic *music, S3MHeader *header)
{
    uint16_t *instrumentParapointers = (uint16_t *)(music->rawData + sizeof(S3MHeader) + header->orderCount);

    music->instrumentCount = header->instrumentCount;
    music->instruments = calloc(sizeof(TrackerMusicInstrument), music->instrumentCount);
    
    if (!music->instruments) {
        printLog("Error: couldn't allocate memory for music instruments!");
        return kMusicMemoryError;
    }
    
    for(int i = 0; i < music->instrumentCount; ++i) {
        S3MInstrument *s3mInst = (S3MInstrument *)parapointerToPointer(music->rawData, instrumentParapointers[i]);
        TrackerMusicInstrument *instrument = &music->instruments[i];
        
        if (s3mInst->length == 0 || s3mInst->type == 0) {
            continue;
        }
        
        if (s3mInst->type != 1) {
            printLog("Error: only PCM instruments are supported. (Instrument %d is type %d)", i + 1, s3mInst->type);
            return kMusicUnsupportedS3MError;
        }
        
        bool isLooping = (s3mInst->flags & S3M_LOOPING_FLAG) != 0;
        bool isStereo = (s3mInst->flags & S3M_STEREO_FLAG) != 0;
        bool is16Bit = (s3mInst->flags & S3M_16_BIT_FLAG) != 0;
        instrument->bytesPerSample = 1;
        
        if (is16Bit) {
            instrument->sampleByteCount = s3mInst->length * 2;
            instrument->bytesPerSample *= 2;
            
            if (isStereo) {
                instrument->format = kSound16bitStereo;
                instrument->bytesPerSample *= 2;
            } else {
                instrument->format = kSound16bitMono;
            }
        } else {
            instrument->sampleByteCount = s3mInst->length;
            
            if (isStereo) {
                instrument->format = kSound8bitStereo;
                instrument->bytesPerSample *= 2;
            } else {
                instrument->format = kSound8bitMono;
            }
        }

        instrument->sampleData = (uint8_t *)parapointerToPointer(music->rawData, (((uint32_t)s3mInst->dataPtrHi) << 16)
                                                                                     | (uint32_t)s3mInst->dataPtrLo);

        if (!(s3mInst->flags & 4)) {
            // Convert to signed 8-bit PCM:
            for (uint32_t s = 0; s < s3mInst->length; ++s) {
                instrument->sampleData[s] = instrument->sampleData[s] ^ 0x80;
            }
        } else {
            uint16_t *sample16 = (uint16_t *)instrument->sampleData;
            
            for (uint32_t s = 0; s < s3mInst->length; ++s) {
                sample16[s] = sample16[s] ^ 0x8000;
            }
        }
        
        instrument->sampleRate = s3mInst->c4Rate;
        instrument->volume = s3mInst->volume;
        
        if (isLooping) {
            instrument->loopBegin = s3mInst->loopBegin;
            instrument->loopEnd = s3mInst->loopEnd;
        }
    }
    
    return kMusicNoError;
}

int loadMusicFromS3M(TrackerMusic *music, char *path, FileOptions mode)
{
    FileStat stat;
    SDFile *f;
    S3MHeader *header;
    int error = kMusicNoError;
    
    printLogVerbose("Loading: %s", path);
    
    memset(music, 0, sizeof(TrackerMusic));
    
    if (pd->file->stat(path, &stat) != 0) {
        printLog("Error: couldn't stat file at: %s", path);
        return kMusicFileError;
    }
    
    music->size = stat.size;
    music->rawData = malloc(music->size);
    
    if (!music->rawData) {
        printLog("Error: couldn't malloc music data!");
        freeTrackerMusic(music);
        return kMusicMemoryError;
    }
    
    f = pd->file->open(path, mode);
    
    if (!f) {
        printLog("Error: failed to read s3m at path %s due to error: %s", path, pd->file->geterr());
        freeTrackerMusic(music);
        return kMusicFileError;
    }
    
    if (pd->file->read(f, music->rawData, music->size) != music->size) {
        printLog("Error: did not read the expected number of bytes from s3m at path %s due to error: %s", path,
                 pd->file->geterr());
        freeTrackerMusic(music);
        return kMusicFileError;
    }
    
    pd->file->close(f);
    
    header = (S3MHeader *)music->rawData;
    
    if (header->magicNumber1 != S3M_HEADER_MAGIC_1) {
        printLog("Error: s3m magic number in header is incorrect: %x", header->magicNumber1);
        return kMusicInvalidS3MError;
    }
    
    if (memcmp(header->magicNumber2, S3M_HEADER_MAGIC_2, sizeof(S3M_HEADER_MAGIC_2) - 1)) {
        printLog("Error: s3m magic number 2 in header is incorrect");
        return kMusicInvalidS3MError;
    }

    music->initialSpeed = header->initialSpeed;
    music->initialTempo = header->initialTempo;
    
    music->orderCount = 0;
    music->orders = music->rawData + sizeof(S3MHeader);
    
    error = s3mReadChannels(music, header);
    
    if (error != kMusicNoError) {
        freeTrackerMusic(music);
        return error;
    }
    
    error = s3mReadPatterns(music, header);
    
    if (error != kMusicNoError) {
        freeTrackerMusic(music);
        return error;
    }
    
    error = s3mReadInstruments(music, header);
    
    if (error != kMusicNoError) {
        freeTrackerMusic(music);
        return error;
    }
    
    error = createTrackerMusicAudioEntities(music);
    
    if (error != kMusicNoError) {
        freeTrackerMusic(music);
        return error;
    }
    
    return kMusicNoError;
}
