#ifndef TRACKER_MUSIC_H
#define TRACKER_MUSIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "pd_api.h"

// If you know the music you're playing is going to use less than 32 channels,
// then you can define this ahead of time as a smaller value to save some memory
// and CPU cycles
#ifndef TRACKER_MUSIC_MAX_CHANNELS
#define TRACKER_MUSIC_MAX_CHANNELS 32
#endif

#define TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT 3

typedef struct _TrackerMusicChannelSynth TrackerMusicChannelSynth;

enum {
    kSignalModeNone = 0,
    kSignalModeAdjust,
    kSignalModeAdjustFine,
    kSignalModeWaveform,
    kSignalModeStepped,
    kSignalModeFlipping,
    kSignalModeFluctuating
};

enum {
    kSignalWaveformSine = 0,
    kSignalWaveformSaw = 1,
    kSignalWaveformSquare = 2,
    kSignalWaveformRandom = 3
};

enum {
    kMusicNoError = 0,
    kMusicFileError,
    kMusicMemoryError,
    kMusicTooManyChannelsError,
    kMusicPlaydateSoundError,
    kMusicInvalidS3MError,
    kMusicUnsupportedS3MError,
    kMusicInvalidData,
};

enum {
    kEffectNone = 0,
    kEffectSetGlobalVolume,
    kEffectSetPanning,
    kEffectSetPanningFine,
    kEffectVolumeSlide,
    kEffectPanningSlide,
    kEffectPortamentoUp,
    kEffectPortamentoDown,
    kEffectTonePortamento,
    kEffectVolumeSlideAndTonePortamento,
    kEffectVibrato,
    kEffectVibratoFine,
    kEffectVibratoSetWaveform,
    kEffectVolumeSlideAndVibrato,
    kEffectTremolo,
    kEffectTremoloSetWaveform,
    kEffectTremor,
    kEffectArpeggio,
    kEffectRetrigger,
    kEffectOffset,
    kEffectNoteDelay,
    kEffectSetTempo,
    kEffectSetSpeed,
    kEffectPositionJump,
    kEffectPatternBreak,
};

typedef struct _BaseSignalStepData {
    uint32_t stepStart;
    uint32_t stepEnd;
    uint16_t mode;
    bool set;
    float setValue;
} BaseSignalStepData;

typedef struct _SignalDataHeader {
    _Atomic uint32_t nextStepId;
    uint32_t currentStepId;
    uint32_t processedStepId;
    uint16_t stepDataSize;
    uint16_t currentOffset;
    uint16_t nextOffset;
    float cachedResult;
    float value;
    bool newStep;
} SignalDataHeader;


typedef struct _LinearSignalStepData {
    struct _BaseSignalStepData;
    float adjustment;
} LinearSignalStepData;

typedef struct _LinearSignalData {
    float valueA;
    float valueB;
    float minValue;
    float maxValue;
} LinearSignalData;


typedef struct _SteppedSignalStepData {
    struct _LinearSignalStepData;
    char operator;
    float stepWidth;
} SteppedSignalStepData;


typedef struct _WaveformSignalStepData {
    struct _BaseSignalStepData;
    bool reset;
    float speed;
    float depth;
    uint8_t type;
} WaveformSignalStepData;

typedef struct _WaveformSignalData {
    float positionStart;
    float positionEnd;
} WaveformSignalData;


typedef struct _FlippingSignalStepData {
    struct _BaseSignalStepData;
    bool reset;
    uint32_t onSampleCount;
    uint32_t offSampleCount;
} FlippingSignalStepData;

typedef struct _FlippingSignalData {
    uint32_t lastFlipSample;
    bool lastFlipOn;
} FlippingSignalData;


typedef struct _FluctuatingSignalStepData {
    struct _BaseSignalStepData;
    float values[3];
    uint32_t fluctuationSampleCount;
} FluctuatingSignalStepData;


typedef struct _RetriggerSignalStepData {
    struct _BaseSignalStepData;
    uint32_t retriggerSampleCount;
    uint32_t lastRetriggerSample;
    uint32_t nextRetriggerSample;
    float frequency;
    TrackerMusicChannelSynth *synth;
} RetriggerSignalStepData;


typedef struct _VolumeSignalStepData {
    union {
        BaseSignalStepData base;
        LinearSignalStepData linear;
        SteppedSignalStepData stepped;
        FlippingSignalStepData flipping;
        WaveformSignalStepData waveform;
    };
    bool setGlobalVolume;
    float globalVolume;
} VolumeSignalStepData;

typedef struct _VolumeSignalData {
    SignalDataHeader header;
    LinearSignalData linearData;
    FlippingSignalData flippingData;
    WaveformSignalData waveformData;
    VolumeSignalStepData current;
    VolumeSignalStepData next;
    float globalVolume;
} VolumeSignalData;

typedef struct _RetriggerSignalData {
    SignalDataHeader header;
    RetriggerSignalStepData current;
    RetriggerSignalStepData next;
} RetriggerSignalData;

typedef struct _VolumeAndRetriggerSignalData {
    VolumeSignalData volumeData;
    RetriggerSignalData retriggerData;
} VolumeAndRetriggerSignalData;


typedef struct _PanSignalData {
    SignalDataHeader header;
    LinearSignalData linearData;
    LinearSignalStepData current;
    LinearSignalStepData next;
} PanSignalData;


typedef struct _PitchSignalStepData {
    union {
        BaseSignalStepData base;
        LinearSignalStepData linear;
        WaveformSignalStepData waveform;
        FluctuatingSignalStepData fluctuating;
    };
    float frequency;
    float targetFrequency;
} PitchSignalStepData;

typedef struct _PitchSignalData {
    SignalDataHeader header;
    LinearSignalData linearData;
    WaveformSignalData waveformData;
    PitchSignalStepData current;
    PitchSignalStepData next;
    float sampleRate;
    float frequency;
    float targetFrequency;
} PitchSignalData;

typedef struct _PatternCell {
    uint8_t what;
    uint8_t instrument;
    uint8_t note;
    uint8_t volume;
    uint8_t effect;
    uint8_t effectVal;
} PatternCell;

typedef struct _TrackerMusicInstrument {
    uint8_t *sampleData;
    AudioSample *sample;
    SoundFormat format;
    uint32_t sampleByteCount;
    uint8_t bytesPerSample;
    uint32_t sampleRate;
    uint32_t loopBegin;
    uint32_t loopEnd;
    uint8_t volume;
    uint8_t *offsetSampleData;
    uint32_t offsetSampleByteCount;
} TrackerMusicInstrument;

typedef struct _TrackerMusicPlaybackData {
    bool paused;
    uint8_t speed;
    uint8_t tempo;
    uint32_t nextStepSample;
    uint32_t nextNextStepSample;
    uint8_t nextOrderIndex;
    uint8_t nextNextOrderIndex;
    uint8_t nextRow;
    uint8_t nextNextRow;
    uint32_t samplesPerStep;
    TrackerMusicChannelSynth * lastSynth[TRACKER_MUSIC_MAX_CHANNELS];
    bool lastSynthIsRetrigger[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastNote[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastPlayedNote[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastInstrument[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastPlayedInstrument[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastVolume[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastEffect[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastEffectVal[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastPan[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastPanningSlide[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastTonePortamento[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastVibrato[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t lastOffset[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t vibratoWaveform[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t tremoloWaveform[TRACKER_MUSIC_MAX_CHANNELS];
    VolumeAndRetriggerSignalData volumeAndRetriggerSignalData[TRACKER_MUSIC_MAX_CHANNELS];
    PanSignalData panSignalData[TRACKER_MUSIC_MAX_CHANNELS];
    PitchSignalData pitchSignalData[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t pitchSignalOffSteps[TRACKER_MUSIC_MAX_CHANNELS];
    bool pitchSignalValueIsZero[TRACKER_MUSIC_MAX_CHANNELS];
} TrackerMusicPlaybackData;

typedef struct _TrackerMusicChannelSynth {
    PDSynth *synth;
    AudioSample *sample;
    uint8_t instrument;
    uint32_t offset;
    float lastNoteOnFreq; // NB: may only be safely accessed from the Playdate audio thread
    uint32_t lastNoteOn; // NB: can only be accessed while mutex is set
    uint32_t lastNoteOff; // NB: can only be accessed while mutex is set
    atomic_flag mutex;
} TrackerMusicChannelSynth;

typedef struct _TrackerMusicChannel {
    bool enabled;
    SoundChannel *soundChannel;
    PDSynthSignal *volumeController;
    PDSynthSignal *panController;
    PDSynthSignal *pitchController;
    PDSynthSignal *currentPitchController;
    TrackerMusicChannelSynth synths[TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT];
    uint8_t pan;
} TrackerMusicChannel;

typedef struct _TrackerMusic {
    uint8_t *rawData;
    unsigned int size;
    uint8_t initialSpeed;
    uint8_t initialTempo;
    
    uint16_t orderCount;
    uint8_t *orders;
    
    uint16_t patternCount;
    PatternCell *patterns;
    
    uint16_t instrumentCount;
    TrackerMusicInstrument *instruments;
    
    TrackerMusicChannel channels[TRACKER_MUSIC_MAX_CHANNELS];
    uint8_t channelCount;
    
    TrackerMusicPlaybackData pb;
} TrackerMusic;

void initializeTrackerMusic(PlaydateAPI *inAPI);
void playTrackerMusic(TrackerMusic *music, uint32_t when);
void freeTrackerMusic(TrackerMusic *music);
void processTrackerMusicCycle(void);
void stopTrackerMusicAt(uint32_t sample);
void stopTrackerMusic(void);
void setTrackerMusicVolume(float vol);
void setTrackerMusicPaused(bool paused);
void setTrackerMusicPosition(uint8_t orderIndex, uint8_t row);
void getTrackerMusicPosition(uint8_t *orderIndex, uint8_t *row);
void setTrackerMusicSpeed(float speed);
void setTrackerMusicPitchShift(float pitch);

#endif // TRACKER_MUSIC_H
