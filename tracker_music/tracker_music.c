#include "tracker_music.h"

#include "s3m.h"
#include "tracker_music_p.h"

#define MIN(a, b) ((a < b) ? a : b)
#define MAX(a, b) ((a > b) ? a : b)
#define kAudioSampleRate 44100
#define kInstrumentReleaseTime 0.015f
#define kNoteOffLeeway 1000
#define kVolumeScale 0.125f
#define kMinimumLoopSamples 1024
#define kPitchSignalOffStepsThreshold 2

#ifndef PLAYDATE_API_VERSION
// NB: If PLAYDATE_API_VERSION isn't defined and set to the Playdate API's
// current version, then all bug fixes for earlier versions of the API will be
// compiled in.
#define PLAYDATE_API_VERSION 0
#endif

static float volumeSignalStep(VolumeSignalData *data, int *ioSamples, float *interframeVal);
static float panSignalStep(void *userData, int *ioSamples, float *interframeVal);
static float pitchSignalStep(void *userData, int *ioSamples, float *interframeVal);
static void retriggerSignalStep(RetriggerSignalData *data, int ioSamples);
static float volumeAndRetriggerSignalStep(void *userData, int *ioSamples, float *interframeVal);
static void setPanValue(TrackerMusic *music, uint8_t channel, float value);
static void setPanLinearSignal(TrackerMusic *music, uint8_t channel, uint16_t mode, float value);
static bool createInstrumentSynth(TrackerMusic *music, uint8_t channel, TrackerMusicChannelSynth *synth);
static void createOffsetSample(TrackerMusic *music, int instIndex);
static void createFixedLoopSample(TrackerMusicInstrument *instrument);
static void updateTempo(TrackerMusic *music);


#define printLog pd->system->logToConsole
#if TRACKER_MUSIC_VERBOSE
#define printLogVerbose pd->system->logToConsole
#else
#define printLogVerbose(...)
#endif
static PlaydateAPI *pd = NULL;

static TrackerMusic *currentMusic = NULL;
static float speedFactor = 1.0f;
static _Atomic float pitchFactor = 0.0f;


void initializeTrackerMusic(PlaydateAPI *inAPI)
{
    pd = inAPI;
    initializeS3M(inAPI);
}

static inline short clamp(short val, short minVal, short maxVal)
{
    if (val < minVal)
        return minVal;
    if (val > maxVal)
        return maxVal;
    return val;
}

static inline float clampf(float val, float minVal, float maxVal)
{
    if (val < minVal)
        return minVal;
    if (val > maxVal)
        return maxVal;
    return val;
}

static inline float lerp(float u, float a, float b)
{
    return (b - a) * u + a;
}

static inline float changeRange(float val, float oldMin, float oldMax, float newMin, float newMax)
{
    return (val - oldMin) / (oldMax - oldMin) * (newMax - newMin) + newMin;
}

static inline short modulo(short n, short M)
{
    // Implementation of Python-style modulo that returns positive numbers for negative values of n
    return ((n % M) + M) % M;
}

static void lockMutex(atomic_flag *lock)
{
    while(atomic_flag_test_and_set(lock)) {
        // wait for mutex to unlock
    }
}

static void unlockMutex(atomic_flag *lock)
{
    atomic_flag_clear(lock);
}

static inline bool isPlayableNote(uint8_t note) {
    return note > 0 && note != UNSET && note != NOTE_OFF;
}

static inline bool cellHasVolume(PatternCell *cell) {
    return (cell->what & VOLUME_FLAG) != 0 && cell->volume >= 0 && cell->volume <= 0x40;
}

static void updateTempo(TrackerMusic *music)
{
    music->pb.samplesPerStep = lroundf(((float)kAudioSampleRate)
                                       / (4.0f * ((float)music->pb.tempo) * (6.0f / ((float)music->pb.speed)) / 60.0f));
    music->pb.nextNextStepSample = music->pb.nextStepSample + music->pb.samplesPerStep;
    //printLogVerbose("New samples per step: %ld", music->pb.samplesPerStep);
}

static int createMusicChannels(TrackerMusic *music)
{
    for(int i = 0; i < music->channelCount; ++i) {
        if (!music->channels[i].enabled) {
            continue;
        }
        
        music->channels[i].soundChannel = pd->sound->channel->newChannel();

        if (!music->channels[i].soundChannel) {
            printLog("Error: couldn't create SoundChannel");
            return kMusicPlaydateSoundError;
        }

        music->channels[i].volumeController =
            pd->sound->signal->newSignal(volumeAndRetriggerSignalStep, NULL, NULL, NULL,
                                         &music->pb.volumeAndRetriggerSignalData[i]);

        if (!music->channels[i].volumeController) {
            printLog("Error: couldn't create volume PDSynthSignal for channel");
            return kMusicPlaydateSoundError;
        }

        music->channels[i].panController =
            pd->sound->signal->newSignal(panSignalStep, NULL, NULL, NULL, &music->pb.panSignalData[i]);

        if (!music->channels[i].panController) {
            printLog("Error: couldn't create panning PDSynthSignal for channel");
            return kMusicPlaydateSoundError;
        }

        music->channels[i].pitchController =
            pd->sound->signal->newSignal(pitchSignalStep, NULL, NULL, NULL, &music->pb.pitchSignalData[i]);

        if (!music->channels[i].pitchController) {
            printLog("Error: couldn't create PDSynthSignal for channel pitch controller");
            return kMusicPlaydateSoundError;
        }
        
        for(int j = 0; j < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++j) {
            music->channels[i].synths[j].instrument = UNSET;
            music->channels[i].synths[j].sample = 0;
            music->channels[i].synths[j].synth = NULL;
        }
        
        if (!createInstrumentSynth(music, i, &music->channels[i].synths[0])) {
            return kMusicPlaydateSoundError;
        }
        
        if (!createInstrumentSynth(music, i, &music->channels[i].synths[1])) {
            return kMusicPlaydateSoundError;
        }
    }
    
    return kMusicNoError;
}

static int calculateUsedInstrumentsAndOffsets(TrackerMusic *music)
{
    for(int orderIndex = 0; orderIndex < music->orderCount; ++orderIndex) {
        int patternIndex = music->orders[orderIndex];
        PatternCell *pattern = patternAtIndex(music, patternIndex);
        int row, channel;
        
        for(row = 0; row < 64; ++row) {
            for(channel = 0; channel < music->channelCount; ++channel) {
                if (!music->channels[channel].enabled) {
                    continue;
                }
                
                PatternCell *cell = patternCell(music, pattern, row, channel);
                uint8_t instIndex;
                
                if ((cell->what & NOTE_AND_INST_FLAG) == 0) {
                    continue;
                }
                
                if (cell->instrument == 0) {
                    instIndex = music->pb.lastInstrument[channel];
                } else {
                    instIndex = cell->instrument - 1;
                    music->pb.lastInstrument[channel] = instIndex;
                }
                
                if (instIndex == UNSET) {
                    continue;
                }
                
                if (instIndex >= music->instrumentCount) {
                    printLog("Error: cell has instrument > num instruments");
                    printLog("... pattern: %d  row: %d  channel: %d", patternIndex, row, channel);
                    return kMusicInvalidData;
                }
                
                TrackerMusicInstrument *inst = &music->instruments[instIndex];
                
                if (cell->what & EFFECT_FLAG && cell->effect == kEffectOffset) {
                    if ((inst->loopBegin != 0 || inst->loopEnd != 0) && (cell->effectVal * 256) > inst->loopBegin) {
                        // We're using offsetSampleByteCount as a flag to
                        // indicate when an instrument will likely need an
                        // offset sample created, to do that work ahead of time.
                        inst->offsetSampleByteCount = SYNTH_DATA_UNINITIALIZED;
                    }
                }
            }
        }
    }
    
    return kMusicNoError;
}

static int createMusicInstruments(TrackerMusic *music)
{
    for(int i = 0; i < music->instrumentCount; ++i) {
        TrackerMusicInstrument *instrument = &music->instruments[i];
        bool isStereo = SoundFormatIsStereo(instrument->format);
        
        // Due to a bug in the Playdate 2.5.0 API, looping samples whose loop
        // length less than a certain number of samples -- say around 500 --
        // will play with horrible distortion at higher notes. This bit here
        // recreates the sample so that it's loop is extended until it is at
        // least kMinimumLoopSamples long in order to work around the issue.
        // This bug is fixed in API 2.6.0, so we check the current API version
        // and conditionally include the fix if it's needed. (See
        // playdate-tracker/demo/CMakeLists.txt for an explanation of how this
        // macro is defined and how its value corresponds to the API version.)
#if PLAYDATE_API_VERSION < 20600
        if ((instrument->loopEnd != 0 || instrument->loopBegin != 0)
            && (instrument->loopEnd - instrument->loopBegin) < kMinimumLoopSamples) {
            printLogVerbose("Note: creating fixed looping sample for instrument %d", i);
            createFixedLoopSample(instrument);
        }
#endif
        
        instrument->sample = pd->sound->sample->newSampleFromData(instrument->sampleData, instrument->format,
                                                                  instrument->sampleRate / (isStereo ? 2 : 1),
                                                                  instrument->sampleByteCount, 0);

        if (!instrument->sample) {
            printLog("Error: couldn't create AudioSample for instrument %d", i + 1);
            return kMusicPlaydateSoundError;
        }
        
        if (instrument->offsetSampleByteCount == SYNTH_DATA_UNINITIALIZED) {
            createOffsetSample(music, i);
        }
    }
    
    return kMusicNoError;
}

int createTrackerMusicAudioEntities(TrackerMusic *music)
{
    int error;
    
    error = createMusicChannels(music);
    
    if (error != kMusicNoError) {
        return error;
    }
    
    error = calculateUsedInstrumentsAndOffsets(music);
    
    if (error != kMusicNoError) {
        return error;
    }
    
    error = createMusicInstruments(music);
    
    if (error != kMusicNoError) {
        return error;
    }
    
    return kMusicNoError;
}

static bool createInstrumentSynth(TrackerMusic *music, uint8_t channel, TrackerMusicChannelSynth *synth)
{
    synth->synth = pd->sound->synth->newSynth();
    
    if (!synth->synth) {
        printLog("Error: couldn't create PDSynth");
        return false;
    }
    
    pd->sound->synth->setAttackTime(synth->synth, 0.0f);
    pd->sound->synth->setReleaseTime(synth->synth, kInstrumentReleaseTime);
    
    if (music->channels[channel].currentPitchController != NULL) {
        pd->sound->synth->setFrequencyModulator(synth->synth,
                                                (PDSynthSignalValue *)music->channels[channel].currentPitchController);
    }
    
    pd->sound->channel->addSource(music->channels[channel].soundChannel, (SoundSource *)synth->synth);

    synth->offset = 0;
    synth->instrument = UNSET;
    synth->sample = NULL;
    
    return true;
}

static void createOffsetSample(TrackerMusic *music, int instIndex)
{
    TrackerMusicInstrument *instrument = &music->instruments[instIndex];
    
    printLogVerbose("Note: creating offset sample for instrument %d", instIndex);
    
    uint32_t loopLength = (instrument->loopEnd - instrument->loopBegin) * instrument->bytesPerSample;
    instrument->offsetSampleData = malloc(loopLength * 2);
    memcpy(instrument->offsetSampleData, instrument->sampleData + instrument->loopBegin * instrument->bytesPerSample,
            loopLength);
    memcpy(instrument->offsetSampleData + loopLength,
           instrument->sampleData + instrument->loopBegin * instrument->bytesPerSample, loopLength);
    instrument->offsetSampleByteCount = 2 * loopLength;
}

#if PLAYDATE_API_VERSION < 20600

static void createFixedLoopSample(TrackerMusicInstrument *instrument)
{
    uint32_t oldLoopLength = instrument->loopEnd - instrument->loopBegin;
    uint32_t repeatCount = (kMinimumLoopSamples / oldLoopLength) + 1;
    uint32_t newSampleLength = (instrument->loopBegin + repeatCount * oldLoopLength);
    
    uint8_t *fixedSample = malloc(newSampleLength * instrument->bytesPerSample);
    
    memcpy(fixedSample, instrument->sampleData, instrument->loopBegin * instrument->bytesPerSample);
    
    for(uint32_t j = 0; j < repeatCount; ++j) {
        memcpy(fixedSample + (instrument->loopBegin + j * oldLoopLength) * instrument->bytesPerSample,
                instrument->sampleData + instrument->loopBegin * instrument->bytesPerSample,
                oldLoopLength * instrument->bytesPerSample);
    }
    
    instrument->loopEnd = newSampleLength;
    instrument->sampleData = fixedSample;
    instrument->sampleByteCount = newSampleLength * instrument->bytesPerSample;
}

#endif

static bool isInRawData(TrackerMusic *music, void *ptr)
{
    return (uint8_t *)ptr >= music->rawData && (uint8_t *)ptr < (music->rawData + music->size);
}

void freeTrackerMusic(TrackerMusic *music)
{
    int i, j;
    
    if (currentMusic == music) {
        stopTrackerMusic();
    }
    
    printLogVerbose("Freeing music");
    
    if (music->instruments) {
        for(i = 0; i < music->instrumentCount; ++i) {
            if (music->instruments[i].sampleData) {
                if (!isInRawData(music, music->instruments[i].sampleData)) {
                    free(music->instruments[i].sampleData);
                }
            }
            
            if (music->instruments[i].sample) {
                pd->sound->sample->freeSample(music->instruments[i].sample);
            }
            
            if (music->instruments[i].offsetSampleData) {
                free(music->instruments[i].offsetSampleData);
            }
        }
        
        if (!isInRawData(music, music->instruments)) {
            free(music->instruments);
        }
        
        music->instruments = NULL;
    }
    
    for(i = 0; i < TRACKER_MUSIC_MAX_CHANNELS; ++i) {
        if (music->channels[i].volumeController) {
            pd->sound->signal->freeSignal(music->channels[i].volumeController);
            music->channels[i].volumeController = NULL;
        }
        
        if (music->channels[i].panController) {
            pd->sound->signal->freeSignal(music->channels[i].panController);
            music->channels[i].panController = NULL;
        }
        
        if (music->channels[i].pitchController) {
            pd->sound->signal->freeSignal(music->channels[i].pitchController);
        }
        
        for(j = 0; j < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++j) {
            if (music->channels[i].synths[j].synth) {
                pd->sound->synth->freeSynth(music->channels[i].synths[j].synth);
            }
        
            if (music->channels[i].synths[j].sample) {
                pd->sound->sample->freeSample(music->channels[i].synths[j].sample);
            }
        }
        
        if (music->channels[i].soundChannel) {
            pd->sound->channel->freeChannel(music->channels[i].soundChannel);
            music->channels[i].soundChannel = NULL;
        }
        
    }
    
    if (music->patterns) {
        if (!isInRawData(music, music->patterns)) {
            free(music->patterns);
        }
        
        music->patterns = NULL;
    }
    
    if (music->orders) {
        if (!isInRawData(music, music->orders)) {
            free(music->orders);
        }
        
        music->orders = NULL;
    }
    
    if (music->rawData) {
        free(music->rawData);
        music->rawData = NULL;
    }
}

void playTrackerMusic(TrackerMusic *music, uint32_t when)
{
    printLogVerbose("Playing music...");
    stopTrackerMusic();
    
    currentMusic = music;
    memset(&music->pb, 0, sizeof(music->pb));
    uint32_t currentTime = pd->sound->getCurrentTime();
    
    if (when < currentTime) {
        when = currentTime;
    }
    
    speedFactor = 1.0f;
    pitchFactor = 0.0f;
    music->pb.speed = music->initialSpeed;
    music->pb.tempo = music->initialTempo;
    updateTempo(music);
    music->pb.nextStepSample = when;
    music->pb.nextNextStepSample = music->pb.nextStepSample + music->pb.samplesPerStep;
    music->pb.nextOrderIndex = music->pb.nextNextOrderIndex = 0;
    music->pb.nextRow = music->pb.nextNextRow = 0;
    
    memset(music->pb.lastNote, UNSET, sizeof(music->pb.lastNote));
    memset(music->pb.lastPlayedNote, UNSET, sizeof(music->pb.lastPlayedNote));
    memset(music->pb.lastInstrument, UNSET, sizeof(music->pb.lastInstrument));
    memset(music->pb.lastPlayedInstrument, UNSET, sizeof(music->pb.lastPlayedInstrument));
    memset(music->pb.lastVolume, UNSET, sizeof(music->pb.lastVolume));
    memset(music->pb.pitchSignalOffSteps, kPitchSignalOffStepsThreshold, sizeof(music->pb.pitchSignalOffSteps));
    memset(music->pb.pitchSignalValueIsZero, true, sizeof(music->pb.pitchSignalValueIsZero));
    
    for(int i = 0; i < music->channelCount; ++i) {
        if (!music->channels[i].enabled) {
            continue;
        }
        
        pd->sound->channel->setPanModulator(music->channels[i].soundChannel,
                                            (PDSynthSignalValue *)music->channels[i].panController);
        pd->sound->channel->setVolumeModulator(music->channels[i].soundChannel,
                                               (PDSynthSignalValue *)music->channels[i].volumeController);
        music->pb.volumeAndRetriggerSignalData[i].volumeData.globalVolume = 1.0f;
        music->pb.lastPan[i] = music->channels[i].pan;
        setPanValue(music, i, (float)music->channels[i].pan);
    }
}

static uint32_t ticksToSamples(TrackerMusic *music, uint16_t ticks)
{
    return ticks * music->pb.samplesPerStep / music->pb.speed;
}

// We need to be careful about scheduling both note on and note off events for a
// PDSynth. If you schedule a note on event before an already-scheduled note
// off, then it will prevent a future note off from firing, resulting in a stuck
// note. This can also happen (possibly due to a race condition or bug) if the
// note on event is scheduled 500 samples or less after a note off. So to avoid
// that, we don't schedule any note on events within 1000 samples of a note off,
// and instead will use a different PDSynth.

// Because we're accessing lastNoteOn / lastNoteOff in both the audio and main
// threads, we need a locking mechanism. And since these functions are being
// called from the Playdate audio thread, we're going to do as little work as
// possible while the mutex is locked, and be sure not to call any Playdate API
// functions.

static bool _checkNoteOffAndSetNoteOnTime(TrackerMusicChannelSynth *synth, float freq, uint32_t when,
                                          uint32_t currentTime, float *length)
{
    if (currentTime < synth->lastNoteOff) {
        if (when >= synth->lastNoteOff + kNoteOffLeeway) {
            return false;
        }
        
        // Because we already have a note off event scheduled, we can replace it
        // with a note on event of finite length, since we know when we need the
        // note to stop already.
        (*length) = (synth->lastNoteOff - when) / ((float)kAudioSampleRate);
    } else {
        (*length) = -1.0f;
    }
    
    synth->lastNoteOn = when;
    synth->lastNoteOnFreq = freq;
    
    return true;
}

static void playSynthNote(TrackerMusicChannelSynth *synth, float freq, uint32_t when)
{
    uint32_t currentTime = pd->sound->getCurrentTime();
    float length = 0.0f;
    bool canPlay;
    
    lockMutex(&synth->mutex);
    canPlay = _checkNoteOffAndSetNoteOnTime(synth, freq, when, currentTime, &length);
    unlockMutex(&synth->mutex);
    
    if (!canPlay) {
        printLog("Error: tried to play synth when it already has a scheduled note off, or too close to recent note off");
        printLog("    lastNoteOff: %d    when: %d", synth->lastNoteOff, when);
        return;
    }
    
    pd->sound->synth->playNote(synth->synth, freq, 1.0, length, when);
}

static bool _checkNoteOnAndSetNoteOffTime(TrackerMusicChannelSynth *synth, uint32_t when, uint32_t currentTime,
                                          uint32_t *noteOnTime, float *length)
{
    if (pd->sound->getCurrentTime() < synth->lastNoteOn) {
        if (when <= synth->lastNoteOn) {
            return false;
        }
        
        // If a note is already scheduled to play, then rather than scheduling a
        // note off (which won't work -- PDSynth only allows one event to be
        // scheduled at a time) we can schedule a new note on event with a
        // finite duration, making it so that it'll stop playing when we
        // would've scheduled a note off event.
        (*noteOnTime) = synth->lastNoteOn;
        (*length) = (when - synth->lastNoteOn) / ((float)kAudioSampleRate);
    }
    
    synth->lastNoteOff = when;
    
    return true;
}

static void releaseSynthNote(TrackerMusicChannelSynth *synth, uint32_t when)
{
    uint32_t currentTime = pd->sound->getCurrentTime();
    uint32_t noteOnTime = 0;
    float length = 0;
    bool canRelease;
    
    lockMutex(&synth->mutex);
    canRelease = _checkNoteOnAndSetNoteOffTime(synth, when, currentTime, &noteOnTime, &length);
    unlockMutex(&synth->mutex);
    
    if (!canRelease) {
        printLog("Error: tried to release note before the note is already scheduled to play");
        return;
    }
    
    if (noteOnTime != 0) {
        pd->sound->synth->playNote(synth->synth, synth->lastNoteOnFreq, 1.0, length, noteOnTime);
    } else {
        pd->sound->synth->noteOff(synth->synth, when);
    }
}

// These need to be accessed while the synth's mutex is locked:
static void getSynthLastNoteOnAndOffTimes(TrackerMusicChannelSynth *synth, uint32_t *noteOn, uint32_t *noteOff)
{
    lockMutex(&synth->mutex);
    (*noteOn) = synth->lastNoteOn;
    (*noteOff) = synth->lastNoteOff;
    unlockMutex(&synth->mutex);
}

#define SCREAM_TRACKER_AMIGA_CLOCK_RATE 3579264.0f
#define SCREAM_TRACKER_CLOCK_RATE (SCREAM_TRACKER_AMIGA_CLOCK_RATE*4.0f)
#define PERIOD_FREQ_MAGIC_NUMBER 197;

// I'm not sure why these numbers work exactly, or how to simplify it outside
// of just multiplying all the constants together in a way that obscures its
// meaning:
static inline float frequencyToAmigaPeriod(float freq, float sampleRate)
{
    //return (SCREAM_TRACKER_CLOCK_RATE * PERIOD_FREQ_MAGIC_NUMBER * 44100.0f) / (8363.0f * 16.0f * freq * sampleRate);
    return 929002505.162523900573614f / (freq * sampleRate);
}

static inline float amigaPeriodToFrequency(float period, float sampleRate)
{
    return frequencyToAmigaPeriod(period, sampleRate);
}

static bool calculateSignalStep(SignalDataHeader *header, int ioSamples, uint32_t *frameStart, uint32_t *frameEnd)
{
    // A lot of the time these signals aren't going to do anything
    // but output their last value, so if that's the case, we want
    // to figure it out as fast as possible to avoid using extra
    // CPU cycles
    if (header->processedStepId == header->nextStepId) {
        return false;
    }
    
    (*frameStart) = pd->sound->getCurrentTime();
    (*frameEnd) = (*frameStart) + ioSamples;
    
    // Uninitialized
    if (header->stepDataSize == 0) {
        return false;
    }
    
    BaseSignalStepData *current = (BaseSignalStepData *)((uint8_t *)header + header->currentOffset);
    BaseSignalStepData *next = (BaseSignalStepData *)((uint8_t *)header + header->nextOffset);
    
    if ((*frameEnd) >= current->stepEnd && header->processedStepId != header->currentStepId) {
        //printLog("**** currentStep processed %d - %d -> %d", header->currentStepId, current->stepStart, current->stepEnd);
        header->processedStepId = header->currentStepId;
    }
    
    // Handle transition point. Error cases ideally shouldn't happen, but we
    // must assume they are possible and try to handle them as robustly as
    // possible.
    //  |   cur?   |   next   | 
    //           ^  ^
    //  |   cur?   |   next   | 
    //                 ^  ^            <-- error case
    //  |   cue?   |   next   | 
    //                       ^  ^      <-- error case
    //  |   cur?   |   next   | 
    //                           ^  ^  <-- error case
    if (header->currentStepId != header->nextStepId && (*frameEnd) >= next->stepStart) {
        //printLog("**** nextStep begins! %d - %d -> %d", header->nextStepId, next->stepStart, next->stepEnd);
        header->currentStepId = header->nextStepId;
        memcpy(current, next, header->stepDataSize);
        
        // Blank out all the step data except the stepStart, stepEnd:
        next->mode = kSignalModeNone;
        next->set = false;
        next->setValue = 0;
        memset(((uint8_t *)next) + sizeof(BaseSignalStepData), 0, header->stepDataSize - sizeof(BaseSignalStepData));
        
        header->newStep = true;
        
        if (current->set) {
            header->value = current->setValue;
        }
    }
    
    return true;
}

static float calculateLinearSignal(SignalDataHeader *base, LinearSignalData *linearData, LinearSignalStepData *current,
                                   uint32_t frameStart, uint32_t frameEnd, int *ioSamples, bool *setInterframeVal)
{
    uint32_t frameMid = (frameStart + frameEnd) / 2;
    
    if (base->newStep) {
        base->newStep = false;
        
        if (current->set) {
            linearData->valueA = base->value;
            linearData->valueB = base->value;
        }
        
        switch(current->mode) {
            case kSignalModeNone:
                break;
            case kSignalModeAdjust:
                linearData->valueA = clampf(base->value, linearData->minValue, linearData->maxValue);
                linearData->valueB = clampf(base->value + current->adjustment, linearData->minValue, linearData->maxValue);
                break;
            case kSignalModeAdjustFine:
                linearData->valueA = clampf(base->value + current->adjustment, linearData->minValue, linearData->maxValue);
                linearData->valueB = linearData->valueA;
                break;
            default:
                printLog("Error: incorrect mode in CalculateLinearSignal! Mode: %d", current->mode);
                break;
        }
        
        base->value = linearData->valueB;
    }

    // This situations should never happen:
    //       |   cur    |   next?  | 
    //  ^  ^
    
    if (frameEnd >= current->stepEnd) {
        //  |   cur    |
        //            ^  ^
        //  |   cur    |          |   next   |
        //            ^  ^
        if (frameStart < current->stepEnd) {
            (*ioSamples) = current->stepEnd;
            (*setInterframeVal) = true;
            return linearData->valueB;
            
        //  |   cur    |
        //             ^  ^
        //  |   cur    |
        //               ^  ^
        //  |   cur    |          |   next   |
        //               ^  ^
        } else {
            return linearData->valueB;
        }
    }
    
    //    |   cur    |   next?  | 
    //  ^  ^
    if (frameStart < current->stepStart) {
        (*ioSamples) = current->stepStart;
        (*setInterframeVal) = true;
        return linearData->valueA;
    }
    
    //    |   cur    |   next?  | 
    //    ^  ^
    //    |   cur    |   next?  | 
    //       ^  ^
    //    |   cur    |   next?  | 
    //            ^  ^
    if (frameEnd < current->stepEnd) {
        if (linearData->valueA == linearData->valueB) {
            return linearData->valueA;
        }
        
        if (frameMid <= current->stepStart) {
            return linearData->valueA;
        }
        
        if (frameMid >= current->stepEnd) {
            return linearData->valueB;
        }

        return (((float)(frameMid - current->stepStart)) / ((float)(current->stepEnd - current->stepStart)))
                   * (linearData->valueB - linearData->valueA)
               + linearData->valueA;
    }
    
    printLog("Error: Unhandled case in CalculateLinearSignal!");
    printLog("... frame:    %d %d", frameStart, frameEnd);
    printLog("... current:  %d %d", current->stepStart, current->stepEnd);
    return 0.0f;
}

static float calculateWaveformSignal(SignalDataHeader *base, WaveformSignalData *waveformData,
                                     WaveformSignalStepData *current, uint32_t frameStart, uint32_t frameEnd,
                                     int *ioSamples, bool *setInterframeVal)
{
    if (base->newStep) {
        base->newStep = false;
        (*ioSamples) = current->stepStart;
        (*setInterframeVal) = true;
        
        if (current->reset) {
            waveformData->positionStart = 0;
        } else {
            waveformData->positionStart = waveformData->positionEnd;
        }
        
        waveformData->positionEnd = waveformData->positionStart + current->speed;
    }
    
    uint32_t frameMid = (frameStart + frameEnd) / 2;
    
    if (frameMid > current->stepEnd) {
        return 0.0f;
    }
    
    float u = fmodf(changeRange(frameMid, current->stepStart, current->stepEnd, waveformData->positionStart,
                                waveformData->positionEnd), 64.0f);
    
    switch(current->type) {
        case kSignalWaveformSine:
            return sinf(u / 64.0f * 2.0f * ((float)M_PI)) * current->depth * 2.0f;
        case kSignalWaveformSaw:
            return lerp(fmodf(u + 32, 64.0f) / 64.0f, current->depth * 2.0f, -current->depth * 2.0f);
        case kSignalWaveformSquare:
            if (modulo((int)(u / 32.0f), 2) == 0) {
                return current->depth * 2.0f;
            } else {
                return -current->depth * 2.0f;
            }
        case kSignalWaveformRandom: {
            // Who uses random waveforms?! I couldn't get it to work, and it
            // honestly doesn't seem worth implementing.
            /*
            unsigned int seed = (int)(changeRange(frameMid, current->stepStart, current->stepEnd, 0, 1) * 4)
                                + (current->stepStart << 4);
            rand_r(&seed);
            rand_r(&seed);
            rand_r(&seed);
            rand_r(&seed);
            return (((float)rand_r(&seed) / (float)RAND_MAX) * 2.0f - 1.0f) * current->depth * 2.0f;
            */
        }
    }
    
    return 0.0f;
}

static float calculateSteppedSignal(SignalDataHeader *base, SteppedSignalStepData *current, uint32_t frameStart)
{
    float result;
    
    if (current->operator == '0') {
        return base->value;
    }
    
    short step = (frameStart > current->stepStart) ? (short)((frameStart - current->stepStart) / current->stepWidth) : 0;
    
    if (current->operator == '+') {
        result = base->value + current->adjustment * step;
    } else if (current->operator == '*') {
        result = base->value * powf(current->adjustment, (float)step);
    } else {
        result = base->value;
    }
    
    return result;
}

static float calculateFlippingSignal(SignalDataHeader *base, FlippingSignalData *flippingData,
                                     FlippingSignalStepData *current, uint32_t frameStart, uint32_t frameEnd,
                                     int *ioSamples, bool *setInterframeVal)
{
    if (base->newStep) {
        base->newStep = false;
        
        if (current->reset) {
            flippingData->lastFlipSample = current->stepStart;
            flippingData->lastFlipOn = true;
        }
    }
    
    if (frameStart >= current->stepEnd || frameEnd < current->stepStart) {
        return base->value;
    }

    uint32_t nextFlipSample =
        flippingData->lastFlipSample + (flippingData->lastFlipOn ? current->onSampleCount : current->offSampleCount);

    if (frameEnd >= nextFlipSample) {
        flippingData->lastFlipSample = nextFlipSample;
        flippingData->lastFlipOn = !flippingData->lastFlipOn;
        
        if (frameStart < nextFlipSample) {
           (*setInterframeVal) = true;
           (*ioSamples) = nextFlipSample;
        }
    }
    
    return flippingData->lastFlipOn ? base->value : 0.0f;
}

static float calculateFluctuatingSignal(SignalDataHeader *base, FluctuatingSignalStepData *current, uint32_t frameStart,
                                        uint32_t frameEnd, int *ioSamples, bool *setInterframeVal)
{
    if (frameStart >= current->stepEnd || frameEnd < current->stepStart) {
        return base->value;
    }
    
    uint32_t n1 = 0, n2 = 0;
    
    n1 = frameStart / current->fluctuationSampleCount;
    n2 = frameEnd / current->fluctuationSampleCount;
    
    if (n1 == n2) {
        return current->values[n1 % 3];
    } else {
        (*setInterframeVal) = true;
        (*ioSamples) = n2 * current->fluctuationSampleCount;
        return current->values[n2 % 3];
    }
}

static float volumeSignalStep(VolumeSignalData *data, int *ioSamples, float *interframeVal)
{
    uint32_t frameStart = 0, frameEnd = 0;
    bool setInterframeVal = false;
    float result = 0.0f;
    
    if (!calculateSignalStep(&data->header, *ioSamples, &frameStart, &frameEnd)) {
        return data->header.cachedResult;
    }
    
    if (data->header.newStep && data->current.setGlobalVolume) {
        data->globalVolume = data->current.globalVolume;
    }
    
    switch(data->current.base.mode) {
        case kSignalModeNone:
            result = data->header.value;
            break;
        case kSignalModeStepped:
            result = clampf(calculateSteppedSignal(&data->header, (SteppedSignalStepData *)&data->current, frameStart),
                            0.0f, 1.0f);
            break;
        case kSignalModeAdjust:
        case kSignalModeAdjustFine:
            result = calculateLinearSignal(&data->header, &data->linearData, (LinearSignalStepData *)&data->current,
                                        frameStart, frameEnd, ioSamples, &setInterframeVal);
            break;
        case kSignalModeFlipping:
            result = calculateFlippingSignal(&data->header, &data->flippingData, (FlippingSignalStepData *)&data->current,
                                             frameStart, frameEnd, ioSamples, &setInterframeVal);
            break;
        case kSignalModeWaveform:
            result = calculateWaveformSignal(&data->header, &data->waveformData, (WaveformSignalStepData *)&data->current,
                                            frameStart, frameEnd, ioSamples, &setInterframeVal) + data->header.value;
            break;
        default:
            printLog("Error: Unhandled volume mode! %d", data->current.base.mode);
            break;
    }
    
    data->header.cachedResult = result * data->globalVolume;
    
    if (setInterframeVal) {
        (*interframeVal) = data->header.cachedResult;
    }
    
    return data->header.cachedResult;
}

static void retriggerSignalStep(RetriggerSignalData *data, int ioSamples)
{
    uint32_t frameStart = 0, frameEnd = 0;
    
    if (!calculateSignalStep(&data->header, ioSamples, &frameStart, &frameEnd)) {
        return;
    }
    
    RetriggerSignalStepData *current = &data->current;
    
    if (frameStart >= current->stepEnd) {
        return;
    }
    
    if (frameStart < current->lastRetriggerSample || current->nextRetriggerSample >= current->stepEnd) {
        return;
    }

    // We're being naughty here and using the audio thread to schedule playing
    // notes, since we can't rely on processTrackerMusicCycle() being called
    // quickly enough. (Is this allowed?)
    playSynthNote(current->synth, current->frequency, current->nextRetriggerSample);
    
    current->lastRetriggerSample = current->nextRetriggerSample;
    current->nextRetriggerSample += current->retriggerSampleCount;
}

static float volumeAndRetriggerSignalStep(void *userData, int *ioSamples, float *interframeVal)
{
    VolumeAndRetriggerSignalData *data = (VolumeAndRetriggerSignalData *)userData;
    
    retriggerSignalStep(&data->retriggerData, *ioSamples);
    return volumeSignalStep(&data->volumeData, ioSamples, interframeVal);
}

static float panSignalStep(void *userData, int *ioSamples, float *interframeVal)
{
    PanSignalData *data = (PanSignalData *)userData;
    uint32_t frameStart = 0, frameEnd = 0;
    
    if (!calculateSignalStep(&data->header, *ioSamples, &frameStart, &frameEnd)) {
        return data->header.cachedResult;
    }
    
    bool setInterframeVal = false;
    float result = calculateLinearSignal(&data->header, &data->linearData, &data->current,
                                         frameStart, frameEnd, ioSamples, &setInterframeVal);
    data->header.cachedResult = clampf((result - 128.0f) / 128.0f, -1.0f, 1.0f);
    
    if (setInterframeVal) {
        (*interframeVal) = data->header.cachedResult;
    }
    
    return data->header.cachedResult;
}

static float pitchSignalStep(void *userData, int *ioSamples, float *interframeVal)
{
    PitchSignalData *data = (PitchSignalData *)userData;
    uint32_t frameStart = 0, frameEnd = 0;
    
    if (!calculateSignalStep(&data->header, *ioSamples, &frameStart, &frameEnd)) {
        return data->header.cachedResult + pitchFactor;
    }
    
    // The way we store the cached result is a bit unusual here. Because pitch
    // effects based on a waveform or fluctuating signal don't change the pitch
    // of a channel -- that is, once the effect is over the pitch reverts back
    // to whatever it was given the last played note and any portamento effects
    // -- we don't actually want to cache the results of those calculations. But
    // we do want to cache everything else, since that does effect the channel's
    // pitch. Hence resultPtr, which is where the result should go, and it'll be
    // either the result cache or a local variable depending on whether or not
    // the result should actually be cached.
    
    float resultPeriods = 0, nonCachedResult = 0;
    float *resultPtr = &data->header.cachedResult;
    bool setInterframeVal = false;
    PitchSignalStepData *current = (PitchSignalStepData *)((uint8_t *)userData + data->header.currentOffset);
    
    switch(current->base.mode) {
        case kSignalModeNone:
            break;
        case kSignalModeWaveform:
            resultPtr = &nonCachedResult; // Don't cache this result
            resultPeriods =
                calculateWaveformSignal(&data->header, &data->waveformData, (WaveformSignalStepData *)&data->current,
                                        frameStart, frameEnd, ioSamples, &setInterframeVal)
                + data->header.value;
            break;
        case kSignalModeAdjust:
        case kSignalModeAdjustFine: {
            if (current->frequency == 0) {
                resultPeriods = 0.0f;
                break;
            }
            
            float targetPeriod =
                (current->targetFrequency != 0)
                    ? (frequencyToAmigaPeriod(current->targetFrequency, data->sampleRate)
                    - frequencyToAmigaPeriod(current->frequency, data->sampleRate))
                    : 0;
            
            if (current->targetFrequency != 0 && data->header.newStep) {
                if (data->header.value < targetPeriod) {
                    current->linear.adjustment = fabsf(current->linear.adjustment);
                } else {
                    current->linear.adjustment = -fabsf(current->linear.adjustment);
                }
            }

            resultPeriods =
                calculateLinearSignal(&data->header, &data->linearData, (LinearSignalStepData *)&data->current,
                                      frameStart, frameEnd, ioSamples, &setInterframeVal);

            if (current->targetFrequency != 0
                && (current->linear.mode == kSignalModeAdjust || current->linear.mode == kSignalModeAdjustFine)) {
                if (current->linear.adjustment > 0) {
                    resultPeriods = MIN(resultPeriods, targetPeriod);
                } else {
                    resultPeriods = MAX(resultPeriods, targetPeriod);
                }
                
                data->header.value = resultPeriods;
            }
            
            break;
        }
        case kSignalModeFluctuating:
            resultPtr = &nonCachedResult; // Don't cache this result
            resultPeriods = calculateFluctuatingSignal(&data->header, (FluctuatingSignalStepData *)&data->current, frameStart,
                                                frameEnd, ioSamples, &setInterframeVal)
                     + data->header.value;
            break;
        default:
            printLog("Error: unhandled signal type in PitchSignalStep: %d", current->base.mode);
            break;
    }
    
    data->header.newStep = false;

    if (resultPeriods == 0.0f) {
        data->header.cachedResult = 0.0f;
        return pitchFactor;
    }
    
    float currentPitchPeriod = frequencyToAmigaPeriod(current->frequency, data->sampleRate);
    float newPitchPeriod = clampf(currentPitchPeriod + resultPeriods, 1, 2000);
    float newFrequency = amigaPeriodToFrequency(newPitchPeriod, data->sampleRate);
    
    (*resultPtr) = log2f(newFrequency / current->frequency);
    
    if (setInterframeVal) {
        (*interframeVal) = (*resultPtr);
    }
    
    return (*resultPtr) + pitchFactor;
}

static void setNextBaseSignalData(TrackerMusic *music, SignalDataHeader *header, BaseSignalStepData *current,
                                  BaseSignalStepData *next, uint16_t stepDataSize)
{
    header->stepDataSize = stepDataSize;
    header->currentOffset = (uint8_t *)current - (uint8_t *)header;
    header->nextOffset = (uint8_t *)next - (uint8_t *)header;
    
    next->stepStart = music->pb.nextStepSample;
    next->stepEnd = music->pb.nextNextStepSample;
}

static void maybeIncrementSignalDataStepId(TrackerMusic *music, SignalDataHeader *header, BaseSignalStepData *next)
{
    if (next->stepStart != music->pb.nextStepSample) {
        return;
    }
    
    // nextStepId should always be the last thing that gets changed, since it's
    // what's being used to check if the "next" data is ready to be copied to the
    // current data in the effects processing thread. Assignment to it should be
    // atomic!
    
    // newId might overflow and wrap back to 0, which indicates uninitialized,
    // so we need to make sure we never set it to that. The IDs just let
    // VolumeSignalStep know that there's new data to process by having
    // nextStepId be a different value than currentStepId
    
    uint16_t newId = header->nextStepId + 1;
    
    if (newId == 0) {
        newId = 1;
    }
    
    header->nextStepId = newId;
}

static void setNextSignalValue(TrackerMusic *music, SignalDataHeader *header, BaseSignalStepData *current,
                               BaseSignalStepData *next, uint16_t stepDataSize, float value)
{
    next->set = true;
    next->setValue = value;
    setNextBaseSignalData(music, header, (BaseSignalStepData *)current, (BaseSignalStepData *)next, stepDataSize);
}

static void setNextLinearSignalData(TrackerMusic *music, SignalDataHeader *header, LinearSignalData *linearData,
                                    LinearSignalStepData *current, LinearSignalStepData *next, uint16_t stepDataSize,
                                    uint16_t mode, float value, float minValue, float maxValue)
{
    linearData->minValue = minValue;
    linearData->maxValue = maxValue;
    next->mode = mode;
    next->adjustment = value;

    setNextBaseSignalData(music, header, (BaseSignalStepData *)current, (BaseSignalStepData *)next, stepDataSize);
}

static void setNextWaveformSignalData(TrackerMusic *music, SignalDataHeader *header, WaveformSignalData *waveformData,
                                      WaveformSignalStepData *current, WaveformSignalStepData *next,
                                      uint16_t stepDataSize, float pointsPerTick, float depth, bool reset,
                                      uint8_t waveformType)
{
    next->mode = kSignalModeWaveform;
    next->speed = (music->pb.speed - 1) * pointsPerTick;
    next->depth = depth;
    next->reset = reset;
    next->type = waveformType;
    
    setNextBaseSignalData(music, header, (BaseSignalStepData *)current, (BaseSignalStepData *)next, stepDataSize);
}

static void setNextFluctuatingSignalData(TrackerMusic *music, SignalDataHeader *header,
                                         FluctuatingSignalStepData *current, FluctuatingSignalStepData *next,
                                         uint16_t stepDataSize, float value1, float value2, float value3,
                                         uint32_t sampleCount)
{
    next->mode = kSignalModeFluctuating;
    next->fluctuationSampleCount = sampleCount;
    next->values[0] = value1;
    next->values[1] = value2;
    next->values[2] = value3;
    
    setNextBaseSignalData(music, header, (BaseSignalStepData *)current, (BaseSignalStepData *)next, stepDataSize);
}

// NB: Because we want to avoid as much calculation as we can in the signal
// callback functions, we convert the tracker volume values (0-64) to Playdate
// values (0.0-1.0) now, to avoid having to do a little extra math in the
// callback

static inline float toPlaydateVolume(float val)
{
    return val / 64.0f * kVolumeScale;
}

static inline float toClampedPlaydateVolume(float val)
{
    return clampf(toPlaydateVolume(val), 0.0f, 1.0f);
}

static void setVolumeValue(TrackerMusic *music, uint8_t channel, float value)
{
    VolumeSignalData *data = &music->pb.volumeAndRetriggerSignalData[channel].volumeData;
    setNextSignalValue(music, &data->header, (BaseSignalStepData *)&data->current, (BaseSignalStepData *)&data->next,
                       sizeof(data->current), toClampedPlaydateVolume(value));
}

static void setVolumeLinearSignal(TrackerMusic *music, uint8_t channel, uint16_t mode, float value)
{
    VolumeSignalData *data = &music->pb.volumeAndRetriggerSignalData[channel].volumeData;
    setNextLinearSignalData(music, &data->header, &data->linearData, (LinearSignalStepData *)&data->current,
                            (LinearSignalStepData *)&data->next, sizeof(data->current), mode, toPlaydateVolume(value),
                            0.0f, 1.0f);
}

static void setVolumeWaveformSignal(TrackerMusic *music, uint8_t channel, float speed, float depth, bool reset)
{
    VolumeSignalData *data = &music->pb.volumeAndRetriggerSignalData[channel].volumeData;
    setNextWaveformSignalData(music, &data->header, &data->waveformData, (WaveformSignalStepData *)&data->current,
                              (WaveformSignalStepData *)&data->next, sizeof(data->current), speed,
                              toPlaydateVolume(depth), reset, music->pb.tremoloWaveform[channel]);
}

static void setVolumeSteppedSignal(TrackerMusic *music, uint8_t channel, float stepWidth, char operator, float adjustment)
{
    VolumeSignalData *data = &music->pb.volumeAndRetriggerSignalData[channel].volumeData;
    data->next.base.mode = kSignalModeStepped;
    data->next.stepped.stepWidth = stepWidth;
    data->next.stepped.operator = operator;
    data->next.stepped.adjustment = (operator == '+') ? toPlaydateVolume(adjustment) : adjustment;

    setNextBaseSignalData(music, &data->header, (BaseSignalStepData *)&data->current, (BaseSignalStepData *)&data->next,
                          sizeof(data->current));
}

static void setVolumeFlippingSignal(TrackerMusic *music, uint8_t channel, bool reset, uint8_t onTickCount,
                                    uint8_t offTickCount)
{
    VolumeSignalData *data = &music->pb.volumeAndRetriggerSignalData[channel].volumeData;
    
    data->next.flipping.mode = kSignalModeFlipping;
    data->next.flipping.reset = reset;
    data->next.flipping.onSampleCount = ticksToSamples(music, onTickCount);
    data->next.flipping.offSampleCount = ticksToSamples(music, offTickCount);
    
    setNextBaseSignalData(music, &data->header, (BaseSignalStepData *)&data->current, (BaseSignalStepData *)&data->next,
                          sizeof(data->current));
}

static void setPanValue(TrackerMusic *music, uint8_t channel, float value)
{
    PanSignalData *data = &music->pb.panSignalData[channel];
    setNextSignalValue(music, &data->header, (BaseSignalStepData *)&data->current, (BaseSignalStepData *)&data->next,
                       sizeof(data->current), value);
}

static void setPanLinearSignal(TrackerMusic *music, uint8_t channel, uint16_t mode, float value)
{
    PanSignalData *data = &music->pb.panSignalData[channel];
    
    //printLogVerbose("... update pan chan: %d  mode: %d  value: %f", channel, mode, (double)value);
    setNextLinearSignalData(music, &data->header, &data->linearData, &data->current,
                            (LinearSignalStepData *)&data->next, sizeof(data->current), mode, value, 0, 256);
}

static void setPitchValue(TrackerMusic *music, uint8_t instrument, uint8_t channel, float value)
{
    PitchSignalData *data = &music->pb.pitchSignalData[channel];
    data->sampleRate = music->instruments[instrument].sampleRate;
    
    setNextSignalValue(music, &data->header, (BaseSignalStepData *)&data->current, (BaseSignalStepData *)&data->next,
                       sizeof(data->current), value);
}

static void setPitchLinearSignal(TrackerMusic *music, uint8_t instrument, uint8_t channel, uint16_t mode, float value,
                                 float targetFrequency)
{
    PitchSignalData *data = &music->pb.pitchSignalData[channel];
    
    data->next.frequency = pd_noteToFrequency(music->pb.lastPlayedNote[channel]);
    data->next.targetFrequency = targetFrequency;
    data->sampleRate = music->instruments[instrument].sampleRate;

    setNextLinearSignalData(music, &data->header, &data->linearData, (LinearSignalStepData *)&data->current,
                            (LinearSignalStepData *)&data->next, sizeof(data->current), mode, value, -3000, 3000);
}

static void setPitchWaveformSignal(TrackerMusic *music, uint8_t instrument, uint8_t channel, float speed, float depth,
                                   bool reset)
{
    PitchSignalData *data = &music->pb.pitchSignalData[channel];
    
    data->next.frequency = pd_noteToFrequency(music->pb.lastPlayedNote[channel]);
    data->next.targetFrequency = 0;
    data->sampleRate = music->instruments[instrument].sampleRate;

    data->next.base.set = reset;
    data->next.base.setValue = 0.0f;
    
    //printLogVerbose("... update pitch vibrato chan: %d  speed: %f  depth: %f", channel, (double)speed, (double)depth);
    setNextWaveformSignalData(music, &data->header, &data->waveformData, (WaveformSignalStepData *)&data->current,
                              (WaveformSignalStepData *)&data->next, sizeof(data->current), speed, depth, reset,
                              music->pb.vibratoWaveform[channel]);
}

static void setPitchFluctuationSignal(TrackerMusic *music, int instrument, uint8_t channel, float periods1,
                                      float periods2, uint32_t sampleCount)
{
    PitchSignalData *data = &music->pb.pitchSignalData[channel];
    
    data->next.frequency = pd_noteToFrequency(music->pb.lastPlayedNote[channel]);
    data->next.targetFrequency = 0;
    data->sampleRate = music->instruments[instrument].sampleRate;
    data->next.base.set = true;
    data->next.base.setValue = 0.0f;
    
    //printLogVerbose("... update arpeggio chan: %d   val1: %f   val2:  %f", channel, (double)periods1, (double)periods2);
    setNextFluctuatingSignalData(music, &data->header, (FluctuatingSignalStepData *)&data->current,
                                 (FluctuatingSignalStepData *)&data->next, sizeof(data->current), periods1, periods2, 0,
                                 sampleCount);
}

static void processEffectRetrigger(TrackerMusic *music, uint8_t channel, uint8_t retriggerTicks, uint8_t volumeCommand)
{
    int inst = music->pb.lastPlayedInstrument[channel];
    
    if (inst == UNSET) {
        return;
    }
    
    RetriggerSignalData *retriggerData = &music->pb.volumeAndRetriggerSignalData[channel].retriggerData;
    retriggerData->next.frequency = pd_noteToFrequency(music->pb.lastPlayedNote[channel]);
    retriggerData->next.synth = music->pb.lastSynth[channel];
    music->pb.lastSynthIsRetrigger[channel] = true;
    retriggerData->next.retriggerSampleCount = ticksToSamples(music, MAX(1, retriggerTicks));
    retriggerData->next.lastRetriggerSample = music->pb.nextStepSample;
    retriggerData->next.nextRetriggerSample = music->pb.nextStepSample + retriggerData->next.retriggerSampleCount;
    
    //printLogVerbose("... update retrigger, sample count: %d   next sample: %d", retriggerData->next.retriggerSampleCount,
    //                retriggerData->next.nextRetriggerSample);

    setNextBaseSignalData(music, &retriggerData->header, (BaseSignalStepData *)&retriggerData->current,
                          (BaseSignalStepData *)&retriggerData->next, sizeof(retriggerData->current));
    char operator = 0;
    float adjustment = 0;
    
    switch(volumeCommand) {
        case 0x0:
        case 0x8:
            operator = '0';
            break;
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
            operator = '+';
            adjustment = -(1 << (volumeCommand-1));
            break;
        case 0x6:
            operator = '*';
            adjustment = 0.6666666667f;
            break;
        case 0x7:
            operator = '*';
            adjustment = 0.5f;
            break;
        case 0x9:
        case 0xA:
        case 0xB:
        case 0xC:
        case 0xD:
            operator = '+';
            adjustment = 1 << (volumeCommand-0x9);
            break;
        case 0xE:
            operator = '*';
            adjustment = 1.5f;
            break;
        case 0xF:
            operator = '*';
            adjustment = 2.0f;
            break;
    }
    
    setVolumeSteppedSignal(music, channel, retriggerData->next.retriggerSampleCount, operator, adjustment);
}

static void updateGlobalVolume(TrackerMusic *music, float volume)
{
    for(uint8_t channel = 0; channel < music->channelCount; ++channel) {
        if (!music->channels[channel].enabled) {
            continue;
        }
        
        VolumeSignalData *data = &music->pb.volumeAndRetriggerSignalData[channel].volumeData;
        data->next.globalVolume = clampf(volume / 64.0f, 0.0f, 1.0f); // don't want to multiply this by kVolumeScale!
        data->next.setGlobalVolume = true;

        setNextBaseSignalData(music, &data->header, (BaseSignalStepData *)&data->current,
                              (BaseSignalStepData *)&data->next, sizeof(data->current));
    }
}

// Because PDSynth doesn't (as of the time of writing this) have a feature where
// you can specify a starting time for its sample, in order to implement the
// offset effect, we create new instances of AudioSample that point to different
// points in the sample's data, as stored in memory. However, there's a
// possibility that the offset might go to a point in the middle of a sample's
// loop, in which case there'd be no correct point in memory for creating the
// AudioSample. In that case, what we need is an "offset sample", or a chunk of
// memory that contains the sample's loop twice, one right after the other. That
// way we can create an AudioSample whose loop is set to be the second half of
// the offset sample, and we can have its starting point be anywhere we want in
// the first half of the offset sample, effectively giving us the ability to
// start from anywhere inside its loop.
static void setupSynth(TrackerMusic *music, uint8_t inst, TrackerMusicChannelSynth *synth, uint32_t offset)
{
    TrackerMusicInstrument *instrument = &music->instruments[inst];
    bool isStereo = SoundFormatIsStereo(instrument->format);
    bool isLooping = (instrument->loopBegin != 0 || instrument->loopEnd != 0);
    uint32_t loopBegin = 0, loopEnd = 0;
    
    if (offset * instrument->bytesPerSample >= instrument->sampleByteCount && !isLooping) {
        //printLogVerbose("*** ... overrun!");
        return;
    }
    
    if (pd->sound->synth->isPlaying(synth->synth)) {
        printLog("Warning: tried to adjust sample offset on synth that is still playing -- have to cut off its note");
        pd->sound->synth->stop(synth->synth);
    }
    
    synth->offset = offset;
    synth->instrument = inst;
    
    if (synth->sample) {
        pd->sound->sample->freeSample(synth->sample);
        synth->sample = NULL;
    }
    
    if (offset == 0) {
        pd->sound->synth->setSample(synth->synth, instrument->sample, instrument->loopBegin / (isStereo ? 2 : 1),
                                    instrument->loopEnd / (isStereo ? 2 : 1));
        pd->sound->synth->setAttackTime(synth->synth, 0.0f);
        pd->sound->synth->setReleaseTime(synth->synth, kInstrumentReleaseTime);
        return;
    }
    
    if (isLooping && offset > instrument->loopBegin) {
        if (!instrument->offsetSampleData) {
            // We try to predict which instruments will need an offset sample
            // ahead of time, but without playing through the whole song and
            // taking into account every one of its pattern breaks and position
            // jumps (which might change in real time!) there's no way to be
            // 100% sure. So if needed we create the offset sample as the music
            // is playing. Hopefully it won't cause any performance hiccups!
            printLogVerbose("Note: Creating offset sample for instrument %d on the fly!", inst);
            createOffsetSample(music, inst);
        }
    
        uint32_t offsetLoop = (offset < instrument->loopEnd) ? (offset - instrument->loopBegin) : 0;

        synth->sample =
            pd->sound->sample->newSampleFromData(instrument->offsetSampleData + offsetLoop * instrument->bytesPerSample,
                                                 instrument->format, instrument->sampleRate / (isStereo ? 2 : 1),
                                                 instrument->offsetSampleByteCount
                                                     - offsetLoop * instrument->bytesPerSample,
                                                 0);
        loopBegin = (instrument->loopEnd - instrument->loopBegin) - offsetLoop;
        loopEnd = (instrument->loopEnd - instrument->loopBegin) * 2 - offsetLoop;
        
    } else {
        synth->sample =
            pd->sound->sample->newSampleFromData(instrument->sampleData + offset * instrument->bytesPerSample,
                                                 instrument->format, instrument->sampleRate / (isStereo ? 2 : 1),
                                                 instrument->sampleByteCount - offset * instrument->bytesPerSample, 0);

        if (isLooping) {
            loopBegin = instrument->loopBegin - offset;
            loopEnd = instrument->loopEnd - offset;
        }
    }
    
    if (!synth->sample) {
        printLog("Error: failed to create offset AudioSample!");
        
        if (synth->synth) {
            pd->sound->synth->freeSynth(synth->synth);
            synth->synth = NULL;
        }
        
        return;
    }

    //printLogVerbose("*** setting offset on synth: %p -> %d", synth->synth, offset);
    pd->sound->synth->setSample(synth->synth, synth->sample, loopBegin / (isStereo ? 2 : 1),
                                loopEnd / (isStereo ? 2 : 1));
    pd->sound->synth->setAttackTime(synth->synth, 0.0f);
    pd->sound->synth->setReleaseTime(synth->synth, kInstrumentReleaseTime);
}

static TrackerMusicChannelSynth * selectNextSynthForInstrument(TrackerMusic *music,
                                                                  TrackerMusicInstrument *instrument, uint8_t channel,
                                                                  uint8_t inst, uint32_t offset)
{
    TrackerMusicChannelSynth *availableSynths[TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT] = {0};
    uint8_t availableSynthsCount = 0;
    uint32_t lastNoteOn = 0, lastNoteOff = 0;
    
    for(uint8_t i = 0; i < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++i) {
        // Can't use the last synth if it's involved in a retrigger effect
        if (music->pb.lastSynthIsRetrigger[channel]
            && music->pb.lastSynth[channel] == &music->channels[channel].synths[i]) {
            continue;
        }
        
        getSynthLastNoteOnAndOffTimes(&music->channels[channel].synths[i], &lastNoteOn, &lastNoteOff);
        
        // Can't use a synth that has a note off event, or one that fired recently
        if (pd->sound->getCurrentTime() <= lastNoteOff + kNoteOffLeeway) {
            continue;
        }
        
        // Can't use a synth that has an upcoming note on either
        if (pd->sound->getCurrentTime() <= lastNoteOn) {
            continue;
        }

        // Prioritize synths with matching inst and offsets
        if (music->channels[channel].synths[i].synth && music->channels[channel].synths[i].instrument == inst
            && music->channels[channel].synths[i].offset == offset) {
            return &music->channels[channel].synths[i];
        }

        availableSynths[availableSynthsCount++] = &music->channels[channel].synths[i];
    }
    
    if (availableSynthsCount == 0) {
        printLog("Error: failed to find available PDSynth for channel %d", channel);
        return NULL;
    }
    
    // Failing the above we use the first available synth that's not playing
    
    for(uint8_t i = 0; i < availableSynthsCount; ++i) {
        if (availableSynths[i]->synth && !pd->sound->synth->isPlaying(availableSynths[i]->synth)) {
            return availableSynths[i];
        }
    }
    
    // And failing *that* we use a synth that hasn't been initialized yet
    
    for(uint8_t i = 0; i < availableSynthsCount; ++i) {
        if (!availableSynths[i]->synth) {
            return availableSynths[i];
        }
    }
    
    // And failing *THAT* we just use any synth, and hope for the best! This may result
    // in notes being cut off prematurely
    
    return availableSynths[0];
}

static uint8_t getNextNoteAndStoreLastNote(TrackerMusic *music, uint8_t channel, PatternCell *cell)
{
    if (((cell->what & EFFECT_FLAG) != 0 && cell->effect == kEffectTonePortamento)) {
        // Tone portamento requires some special logic concerning whether we
        // trigger a note, and which note it is
        
        uint8_t noteToPlay = music->pb.lastNote[channel];
        
        if (cell->note != 0) {
            music->pb.lastNote[channel] = cell->note;
        }
        
        // If the instrument is already playing, then we don't want to play
        // another note. We just want the instrument to slide to whatever the
        // last note was.
        if (music->pb.lastSynth[channel] && music->pb.lastSynth[channel]->synth
            && pd->sound->synth->isPlaying(music->pb.lastSynth[channel]->synth)) {
            return UNSET;
        }
        
        // We're not playing a note, so we want to play one now. But we want it
        // to be the last note that was played, so that we can slide into
        // whichever note is specified on the current cell. Since
        // pb.lastNote[channel] defines the note we'll slide towards, this means
        // we want it to have this cell's note, even though that's not the note
        // we're going to play. (Unless there is no last note, in which case we
        // just play the note of the current cell, and the tone portamento
        // effect won't do anything.)
        return (noteToPlay == UNSET) ? cell->note : noteToPlay;
    }
    
    if (cell->note != 0) {
        music->pb.lastNote[channel] = cell->note;
        return cell->note;
    }
    
    return music->pb.lastNote[channel];
}

static void processMusicNote(TrackerMusic *music, uint8_t channel, PatternCell *cell)
{
    uint8_t inst, note;
    
    if (cell->instrument != 0) {
        music->pb.lastInstrument[channel] = cell->instrument - 1;
        
        if (!cellHasVolume(cell)) {
            music->pb.lastVolume[channel] = music->instruments[cell->instrument - 1].volume;
            setVolumeValue(music, channel, (float)music->pb.lastVolume[channel]);
        }
    }
    
    if (cell->note == NOTE_OFF) {
        if (!cellHasVolume(cell)) {
            music->pb.lastVolume[channel] = 0;
            setVolumeValue(music, channel, (float)music->pb.lastVolume[channel]);
        }
        
        if (music->pb.lastSynth[channel]) {
            releaseSynthNote(music->pb.lastSynth[channel], music->pb.nextStepSample);
            music->pb.lastSynth[channel] = NULL;
        }
        
        return;
    }
    
    if (cell->note == UNSET || cell->note == 0) {
        return;
    }
    
    note = getNextNoteAndStoreLastNote(music, channel, cell);
    inst = music->pb.lastInstrument[channel];
    
    if (inst == UNSET || note == UNSET) {
        return;
    }

    uint32_t offset = 0;
    
    if ((cell->what & EFFECT_FLAG) && cell->effect == kEffectOffset) {
        if (cell->effectVal == 0) {
            offset = music->pb.lastOffset[channel] * 256;
        } else {
            offset = cell->effectVal * 256;
            music->pb.lastOffset[channel] = cell->effectVal;
        }
    }

    TrackerMusicChannelSynth *synth =
        selectNextSynthForInstrument(music, &music->instruments[inst], channel, inst, offset);

    if (!synth) {
        printLog("Error: no available PDSynth for instrument %d channel %d!", inst, channel);
        return;
    }
    
    if (!synth->synth) {
        printLogVerbose("Note: Creating synth for instrument %d on the fly!", inst);
        createInstrumentSynth(music, channel, synth);
    }
    
    if (synth->offset != offset || synth->instrument != inst) {
        setupSynth(music, inst, synth, offset);
    }
    
    if (!synth->synth) {
        return;
    }
    
    uint32_t noteTime = music->pb.nextStepSample;
    
    if ((cell->what & EFFECT_FLAG) && cell->effect == kEffectNoteDelay) {
        if (cell->effectVal == 0 || cell->effectVal >= music->pb.speed) {
            return;
        }
        
        noteTime += ticksToSamples(music, cell->effectVal);
    }
    
    if (music->pb.lastSynth[channel] && synth != music->pb.lastSynth[channel]) {
        releaseSynthNote(music->pb.lastSynth[channel], noteTime);
    }
    
    playSynthNote(synth, pd_noteToFrequency(note), noteTime);
    music->pb.lastPlayedNote[channel] = note;
    music->pb.lastPlayedInstrument[channel] = inst;
    
    setPitchValue(music, inst, channel, 0);

    music->pb.lastSynth[channel] = synth;
}

static void processMusicVolume(TrackerMusic *music, uint8_t channel, PatternCell *cell)
{
    //printLogVerbose("... chan %d vol: %02x", channel, cell->volume);
    
    if (cell->volume >= 0x00 && cell->volume <= 0x40) {
        music->pb.lastVolume[channel] = cell->volume;
        setVolumeValue(music, channel, (float)cell->volume);
        
    } else if (cell->volume >= 0x80 && cell->volume <= 0xc0) {
        uint8_t pan = cell->volume - 0x80;
        music->pb.lastPan[channel] = pan;
        setPanValue(music, channel, (float)pan * 4);
    }
}

static void processMusicControlEffect(TrackerMusic *music, PatternCell *cell)
{
    if (cell->effect == 0) {
        return;
    }
    
    switch(cell->effect) {
        case kEffectSetSpeed:
            music->pb.speed = cell->effectVal;
            updateTempo(music);
            break;
        case kEffectPositionJump:
            printLogVerbose("... position jump, to: %d", cell->effectVal);
            music->pb.nextNextOrderIndex = cell->effectVal;
            if (music->pb.nextNextRow == UNSET) {
                music->pb.nextNextRow = 0;
            }
            break;
        case kEffectPatternBreak:
            printLogVerbose("... pattern break, to: %d", cell->effectVal);
            if (music->pb.nextNextOrderIndex == UNSET) {
                music->pb.nextNextOrderIndex = music->pb.nextOrderIndex + 1;
            }
            music->pb.nextNextRow = clamp(cell->effectVal, 0, 63);
            break;
        case kEffectSetTempo:
            if ((cell->effectVal & 0xF0) == 0x00) {
                music->pb.tempo -= cell->effectVal & 0x0F;
            } else if ((cell->effectVal & 0xF0) == 0x10) {
                music->pb.tempo += cell->effectVal & 0x0F;
            } else {
                music->pb.tempo = cell->effectVal;
            }
            updateTempo(music);
            break;
        default:
            break;
    }
}

static void processEffectVolumeSlide(TrackerMusic *music, uint8_t channel, uint8_t effectVal)
{
    effectVal = (effectVal != 0) ? effectVal : music->pb.lastEffectVal[channel];
    
    uint8_t lo = effectVal & 0x0F;
    uint8_t hi = (effectVal & 0xF0) >> 4;
    
    if (hi == 0 && lo != 0) {
        setVolumeLinearSignal(music, channel, kSignalModeAdjust, -((float)lo) * (music->pb.speed - 1));
        
    } else if (lo == 0 && hi != 0) {
        setVolumeLinearSignal(music, channel, kSignalModeAdjust, (float)hi * (music->pb.speed - 1));
        
    } else if (hi == 0xF && lo != 0xF) {
        setVolumeLinearSignal(music, channel, kSignalModeAdjustFine, -((float)lo));
        
    } else if (lo == 0xF && hi != 0xF) {
        setVolumeLinearSignal(music, channel, kSignalModeAdjustFine, (float)hi);
    }
}

static void processEffectPanningSlide(TrackerMusic *music, uint8_t channel, uint8_t effectVal)
{
    if (effectVal != 0) {
        music->pb.lastPanningSlide[channel] = effectVal;
    } else {
        effectVal = music->pb.lastPanningSlide[channel];
    }
    
    uint8_t lo = effectVal & 0x0F;
    uint8_t hi = (effectVal & 0xF0) >> 4;
    
    if (hi == 0 && lo != 0) {
        setPanLinearSignal(music, channel, kSignalModeAdjust, ((float)lo) * 4.0f * (music->pb.speed - 1));
        
    } else if (lo == 0 && hi != 0) {
        setPanLinearSignal(music, channel, kSignalModeAdjust, -(float)hi * 4.0f * (music->pb.speed - 1));
        
    } else if (hi == 0xF && lo != 0xF) {
        setPanLinearSignal(music, channel, kSignalModeAdjustFine, ((float)lo * 4.0f));
        
    } else if (lo == 0xF && hi != 0xF) {
        setPanLinearSignal(music, channel, kSignalModeAdjustFine, -(float)hi * 4.0f);
    }
}

static void processEffectPortamento(TrackerMusic *music, uint8_t channel, uint8_t effectVal, float direction)
{
    if (music->pb.lastPlayedInstrument[channel] == UNSET) {
        return;
    }
    
    effectVal = (effectVal != 0) ? effectVal : music->pb.lastEffectVal[channel];
    
    short lo = effectVal & 0x0F;
    short hi = (effectVal & 0xF0) >> 4;
    
    if (hi == 0x0F) {
        setPitchLinearSignal(music, music->pb.lastPlayedInstrument[channel], channel, kSignalModeAdjustFine,
                              (float)lo * direction, 0);
    } else if (hi == 0x0E) {
        setPitchLinearSignal(music, music->pb.lastPlayedInstrument[channel], channel, kSignalModeAdjustFine,
                              (float)lo * direction / 4.0f, 0);
    } else {
        setPitchLinearSignal(music, music->pb.lastPlayedInstrument[channel], channel, kSignalModeAdjust,
                             (float)(effectVal * direction * (music->pb.speed - 1)), 0);
    }
}

static void processEffectTonePortamento(TrackerMusic *music, uint8_t channel, uint8_t effectVal)
{
    if (music->pb.lastPlayedInstrument[channel] == UNSET) {
        return;
    }
    
    if (effectVal != 0) {
        music->pb.lastTonePortamento[channel] = effectVal;
    } else {
        effectVal = music->pb.lastTonePortamento[channel];
    }
    
    if (music->pb.lastNote[channel] == UNSET || music->pb.lastPlayedNote[channel] == UNSET) {
        return;
    }

    setPitchLinearSignal(music, music->pb.lastPlayedInstrument[channel], channel, kSignalModeAdjust,
                         effectVal * (music->pb.speed - 1), pd_noteToFrequency(music->pb.lastNote[channel]));
}

static void processEffectVibrato(TrackerMusic *music, uint8_t channel, PatternCell *cell, uint8_t effectVal, bool fine)
{
    uint8_t inst = music->pb.lastPlayedInstrument[channel];
    
    if (inst == UNSET) {
        return;
    }
    
    uint8_t lo = effectVal & 0x0F;
    uint8_t hi = (effectVal & 0xF0) >> 4;
    
    if (lo != 0) {
        music->pb.lastVibrato[channel] = (music->pb.lastVibrato[channel] & 0xF0) | lo;
    } else {
        lo = (music->pb.lastVibrato[channel] & 0x0F);
    }
    
    if (hi != 0) {
        music->pb.lastVibrato[channel] = (music->pb.lastVibrato[channel] & 0x0F) | (hi << 4);
    } else {
        hi = (music->pb.lastVibrato[channel] & 0xF0) >> 4;
    }

    setPitchWaveformSignal(music, inst, channel, hi, (float)lo / (fine ? 4.0f : 1.0f),
                           (cell->what & NOTE_AND_INST_FLAG) != 0);
}

static void processEffectTremolo(TrackerMusic *music, uint8_t channel, PatternCell *cell)
{
    uint8_t effectVal = (cell->effectVal != 0) ? cell->effectVal : music->pb.lastEffectVal[channel];
    uint8_t lo = effectVal & 0x0F;
    uint8_t hi = (effectVal & 0xF0) >> 4;
    bool reset = ((cell->what & NOTE_AND_INST_FLAG) != 0 && isPlayableNote(cell->note))
                 || music->pb.lastEffect[channel] != kEffectTremolo;
    uint8_t speed = hi;
    uint8_t depth = lo;

    setVolumeWaveformSignal(music, channel, speed, depth, reset);
}

static void processEffectArpeggio(TrackerMusic *music, uint8_t channel, uint8_t effectVal)
{
    effectVal = (effectVal != 0) ? effectVal : music->pb.lastEffectVal[channel];
    uint8_t lo = effectVal & 0x0F;
    uint8_t hi = (effectVal & 0xF0) >> 4;
    uint8_t inst = music->pb.lastPlayedInstrument[channel];
    
    if (!isPlayableNote(music->pb.lastNote[channel]) || inst == UNSET) {
        return;
    }
    
    float currentFreq = pd_noteToFrequency(music->pb.lastNote[channel]);
    float currentPeriod = frequencyToAmigaPeriod(currentFreq, music->instruments[inst].sampleRate);
    float periods1 = frequencyToAmigaPeriod(currentFreq * powf(2, hi / 12.0f), music->instruments[inst].sampleRate)
                     - currentPeriod;
    float periods2 = frequencyToAmigaPeriod(currentFreq * powf(2, lo / 12.0f), music->instruments[inst].sampleRate)
                     - currentPeriod;
    setPitchFluctuationSignal(music, inst, channel, periods1, periods2, ticksToSamples(music, 1));
}

static void processMusicEffect(TrackerMusic *music, uint8_t channel, PatternCell *cell)
{
    if (cell->effect == 0) {
        music->pb.lastEffect[channel] = 0;
        return;
    }
    
    switch(cell->effect) {
        case kEffectVolumeSlide:
            processEffectVolumeSlide(music, channel, cell->effectVal);
            break;
        case kEffectPortamentoDown: 
            processEffectPortamento(music, channel, cell->effectVal, 1);
            break;
        case kEffectPortamentoUp:
            processEffectPortamento(music, channel, cell->effectVal, -1);
            break;
        case kEffectTonePortamento:
            processEffectTonePortamento(music, channel, cell->effectVal);
            break;
        case kEffectVolumeSlideAndVibrato:
            processEffectVolumeSlide(music, channel, cell->effectVal);
            processEffectVibrato(music, channel, cell, 0, false);
            break;
        case kEffectVolumeSlideAndTonePortamento:
            processEffectVolumeSlide(music, channel, cell->effectVal);
            processEffectTonePortamento(music, channel, 0);
            break;
        case kEffectPanningSlide:
            processEffectPanningSlide(music, channel, cell->effectVal);
            break;
        case kEffectVibratoSetWaveform:
            music->pb.vibratoWaveform[channel] = cell->effectVal;
            break;
        case kEffectTremoloSetWaveform:
            music->pb.tremoloWaveform[channel] = cell->effectVal;
            break;
        case kEffectSetPanning:
            setPanValue(music, channel, (float)cell->effectVal * 16.0f);
            break;
        case kEffectSetPanningFine:
            setPanValue(music, channel, (float)clamp(cell->effectVal, 0, 0x80) * 2.0f);
            break;
        case kEffectVibrato:
            processEffectVibrato(music, channel, cell, cell->effectVal, false);
            break;
        case kEffectVibratoFine:
            processEffectVibrato(music, channel, cell, cell->effectVal, true);
            break;
        case kEffectRetrigger: {
            uint8_t effectVal = (cell->effectVal != 0) ? cell->effectVal : music->pb.lastEffectVal[channel];
            uint8_t lo = effectVal & 0x0F;
            uint8_t hi = (effectVal & 0xF0) >> 4;
            
            if (music->pb.lastSynth[channel]) {
                processEffectRetrigger(music, channel, lo, hi);
            }
            
            break;
        }
        case kEffectTremor: {
            uint8_t effectVal = (cell->effectVal != 0) ? cell->effectVal : music->pb.lastEffectVal[channel];
            uint8_t lo = effectVal & 0x0F;
            uint8_t hi = (effectVal & 0xF0) >> 4;
            bool reset = ((cell->what & NOTE_AND_INST_FLAG) != 0 && isPlayableNote(cell->note))
                         || music->pb.lastEffect[channel] != kEffectTremor;
            setVolumeFlippingSignal(music, channel, reset, hi + 1, lo + 1);
            break;
        }
        case kEffectTremolo:
            processEffectTremolo(music, channel, cell);
            break;
        case kEffectSetGlobalVolume:
            updateGlobalVolume(music, (float)cell->effectVal);
            break;
        case kEffectArpeggio: 
            processEffectArpeggio(music, channel, cell->effectVal);
            break;
        default:
            break;
    }
    
    if (cell->effectVal != 0) {
        music->pb.lastEffectVal[channel] = cell->effectVal;
    }
    
    music->pb.lastEffect[channel] = cell->effect;
}

// PDSynth frequency modulators use a significant amount of CPU time, even when
// they're not actually calculating very much. So this function removes them
// from any synth that doesn't actively need them to save CPU cycles
void setFrequencyModulators(TrackerMusic *music, int channel)
{
    PitchSignalStepData *pitchData = &music->pb.pitchSignalData[channel].next;
    bool signalHolding;
    
    if (pitchData->base.set) {
        music->pb.pitchSignalValueIsZero[channel] = (pitchData->base.setValue == 0.0f);
    }
    
    switch(pitchData->base.mode) {
        case kSignalModeAdjust:
        case kSignalModeAdjustFine:
            music->pb.pitchSignalValueIsZero[channel] = false;
            signalHolding = false;
            break;
        case kSignalModeWaveform:
        case kSignalModeFluctuating:
            signalHolding = false;
            break;
        default:
            signalHolding = true;
    }
    
    if (music->pb.pitchSignalValueIsZero[channel] && signalHolding) {
        music->pb.pitchSignalOffSteps[channel] =
            MIN(music->pb.pitchSignalOffSteps[channel] + 1, kPitchSignalOffStepsThreshold);
    } else {
        music->pb.pitchSignalOffSteps[channel] = 0;
    }
    
    bool enableModulator = (pitchFactor != 0.0f || music->pb.pitchSignalOffSteps[channel] < kPitchSignalOffStepsThreshold);
    
    if (enableModulator && music->channels[channel].currentPitchController == NULL) {
        printLogVerbose("... installing freq modulator for channel: %d", channel);
        music->channels[channel].currentPitchController = music->channels[channel].pitchController;
        
        for(int i = 0; i < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++i) {
            if (music->channels[channel].synths[i].synth) {
                pd->sound->synth->setFrequencyModulator(music->channels[channel].synths[i].synth,
                                                        (PDSynthSignalValue *)music->channels[channel].pitchController);
            }
        }
        
    } else if (!enableModulator && music->channels[channel].currentPitchController != NULL) {
        printLogVerbose("... removing freq modulator for channel: %d", channel);
        music->channels[channel].currentPitchController = NULL;
        
        for(int i = 0; i < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++i) {
            if (music->channels[channel].synths[i].synth) {
                pd->sound->synth->setFrequencyModulator(music->channels[channel].synths[i].synth, NULL);
            }
        }
    }
}

static void calculateUpcomingStepSample(TrackerMusic *music)
{
    if (speedFactor == 1.0f) {
        music->pb.nextNextStepSample = music->pb.nextStepSample + music->pb.samplesPerStep;
    } else {
        music->pb.nextNextStepSample = music->pb.nextStepSample + ((uint32_t)((float)music->pb.samplesPerStep / speedFactor));
    }
}

static void processNextStep(TrackerMusic *music, uint32_t currentTime)
{
    music->pb.nextStepSample = music->pb.nextNextStepSample;
    calculateUpcomingStepSample(music);
    
    if (music->pb.paused) {
        return;
    }
    
    music->pb.nextRow = music->pb.nextNextRow;
    music->pb.nextOrderIndex = music->pb.nextNextOrderIndex;
    
    music->pb.nextNextOrderIndex = UNSET;
    music->pb.nextNextRow = UNSET;

    printLogVerbose("time: %d   processing: %d - order: %d  row: %d", currentTime, music->pb.nextStepSample,
                    music->pb.nextOrderIndex, music->pb.nextRow);

    if (music->pb.nextOrderIndex >= music->orderCount) {
        stopTrackerMusicAt(music->pb.nextStepSample);
        return;
    }

    uint8_t patternIndex = music->orders[music->pb.nextOrderIndex];
    PatternCell *pattern = patternAtIndex(music, patternIndex);//music->patterns[patternIndex];
    
    // Important to process control effects first in case nextNextStepSample changes:
    for(uint8_t channel = 0; channel < music->channelCount; ++channel) {
        if (!music->channels[channel].enabled) {
            continue;
        }
        
        PatternCell *cell = patternCell(music, pattern, music->pb.nextRow, channel);//s3m_get_cell(pattern, channel, music->pb.nextRow);
        
        if ((cell->what & EFFECT_FLAG) != 0) {
            processMusicControlEffect(music, cell);
        }
    }
    
    for(uint8_t channel = 0; channel < music->channelCount; ++channel) {
        if (!music->channels[channel].enabled) {
            continue;
        }
        
        PatternCell *cell = patternCell(music, pattern, music->pb.nextRow, channel);//s3m_get_cell(pattern, channel, music->pb.nextRow);
        
        if ((cell->what & VOLUME_FLAG) != 0) {
            processMusicVolume(music, channel, cell);
        }
        
        if ((cell->what & NOTE_AND_INST_FLAG) != 0) {
            processMusicNote(music, channel, cell);
        }
        
        if ((cell->what & EFFECT_FLAG) != 0) {
            processMusicEffect(music, channel, cell);
        }
    }
    
    if (music->pb.nextNextRow == UNSET || music->pb.nextNextOrderIndex == UNSET) {
        if (music->pb.nextRow < 63) {
            music->pb.nextNextRow = music->pb.nextRow + 1;
            music->pb.nextNextOrderIndex = music->pb.nextOrderIndex;
        } else {
            music->pb.nextNextRow = 0;
            music->pb.nextNextOrderIndex = music->pb.nextOrderIndex + 1;
        }
    }
    
    for(uint8_t channel = 0; channel < music->channelCount; ++channel) {
        if (!music->channels[channel].enabled) {
            continue;
        }
        
        setFrequencyModulators(music, channel);
        maybeIncrementSignalDataStepId(music, &music->pb.volumeAndRetriggerSignalData[channel].volumeData.header,
                                       &music->pb.volumeAndRetriggerSignalData[channel].volumeData.next.base);
        maybeIncrementSignalDataStepId(music, &music->pb.volumeAndRetriggerSignalData[channel].retriggerData.header,
                                       (BaseSignalStepData *)&music->pb.volumeAndRetriggerSignalData[channel]
                                           .retriggerData.next);
        maybeIncrementSignalDataStepId(music, &music->pb.panSignalData[channel].header,
                                       (BaseSignalStepData *)&music->pb.panSignalData[channel].next);
        maybeIncrementSignalDataStepId(music, &music->pb.pitchSignalData[channel].header,
                                       (BaseSignalStepData *)&music->pb.pitchSignalData[channel].next);
    }
}

void processTrackerMusicCycle(void)
{
    TrackerMusic *music = currentMusic;
    
    if (music == NULL) {
        return;
    }
    
    uint32_t currentTime = pd->sound->getCurrentTime();
    
    while(currentTime > music->pb.nextStepSample) {
        processNextStep(music, currentTime);
    }
}

void stopTrackerMusicAt(uint32_t sample)
{
    int i, j;
    
    if (!currentMusic) {
        return;
    }
    
    for(i = 0; i < TRACKER_MUSIC_MAX_CHANNELS; ++i) {
        if (!currentMusic->channels[i].enabled) {
            continue;
        }
        
        pd->sound->channel->setPanModulator(currentMusic->channels[i].soundChannel, NULL);
        pd->sound->channel->setVolumeModulator(currentMusic->channels[i].soundChannel, NULL);
        
        for(j = 0; j < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++j) {
            if (currentMusic->channels[i].synths[j].synth) {
                pd->sound->synth->noteOff(currentMusic->channels[i].synths[j].synth, sample);
                pd->sound->synth->setFrequencyModulator(currentMusic->channels[i].synths[j].synth, NULL);
            }
        }                             
    }
    
    currentMusic = NULL;
}

void stopTrackerMusic(void)
{
    int i, j;
    
    if (!currentMusic) {
        return;
    }
    
    for(i = 0; i < TRACKER_MUSIC_MAX_CHANNELS; ++i) {
        if (!currentMusic->channels[i].enabled) {
            continue;
        }
        
        pd->sound->channel->setPanModulator(currentMusic->channels[i].soundChannel, NULL);
        pd->sound->channel->setVolumeModulator(currentMusic->channels[i].soundChannel, NULL);
        
        for(j = 0; j < TRACKER_MUSIC_INSTRUMENT_PDSYNTH_COUNT; ++j) {
            if (currentMusic->channels[i].synths[j].synth) {
                pd->sound->synth->stop(currentMusic->channels[i].synths[j].synth);
                pd->sound->synth->setFrequencyModulator(currentMusic->channels[i].synths[j].synth, NULL);
            }
        }
    }
    
    currentMusic = NULL;
}

void setTrackerMusicVolume(float vol)
{
    if (!currentMusic) {
        return;
    }
    
    for(int i = 0; i < TRACKER_MUSIC_MAX_CHANNELS; ++i) {
        if (!currentMusic->channels[i].enabled) {
            continue;
        }
        
        pd->sound->channel->setVolume(currentMusic->channels[i].soundChannel, vol);
    }
}

void setTrackerMusicPaused(bool paused)
{
    if (!currentMusic) {
        return;
    }
    
    currentMusic->pb.paused = paused;
}

void setTrackerMusicPosition(uint8_t orderIndex, uint8_t row)
{
    if (!currentMusic) {
        return;
    }
    
    currentMusic->pb.nextNextOrderIndex = orderIndex;
    currentMusic->pb.nextNextRow = clamp(row, 0, 63);
}

void getTrackerMusicPosition(uint8_t *orderIndex, uint8_t *row)
{
    if (!currentMusic) {
        return;
    }
    
    if (orderIndex) {
        *orderIndex = currentMusic->pb.nextOrderIndex;
    }
    
    if (row) {
        *row = currentMusic->pb.nextRow;
    }
}

// Multiplies the normal playback speed by the given value
void setTrackerMusicSpeed(float speed)
{
    if (!currentMusic) {
        return;
    }
    
    speedFactor = clampf(speed, 0.001, 100.0);
    calculateUpcomingStepSample(currentMusic);
}

// Scales the pitch the same way as a frequency modulator: the signal is scaled
// so that a value of 1 doubles the synth pitch (i.e. an octave up) and -1
// halves it (an octave down).
void setTrackerMusicPitchShift(float pitch)
{
    if (!currentMusic) {
        return;
    }
    
    pitchFactor = pitch;
    
    for(uint8_t channel = 0; channel < currentMusic->channelCount; ++channel) {
        if (!currentMusic->channels[channel].enabled) {
            continue;
        }
        
        setFrequencyModulators(currentMusic, channel);
    }
}
