#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "pd_api.h"
#include "s3m.h"
#include "tracker_music.h"

#define MAX_FILES 500
#define MARGINS 5
#define LINE_HEIGHT 30
#define CRANK_SPEED 7
#define CRANK_SMOOTHING 10

#define MIN(a, b) ((a < b) ? a : b)
#define MAX(a, b) ((a > b) ? a : b)

PlaydateAPI *pd = NULL;

const char *infoText = "  A: Play    B: Stop    Crank: Adjust speed";
char *files[MAX_FILES] = {0};
int fileCount = 0;
int selection = 0;
LCDFont *listFont = NULL, *infoFont = NULL;
TrackerMusic currentMusic;
int currentMusicIndex = -1;
float crankValues[CRANK_SMOOTHING] = {0};
int crankValueIndex = 0;


bool caseInsensitiveStrEquals(const char *a, const char *b)
{
    while(1) {
        int d = tolower(*a) - tolower(*b);
        
        if (d != 0 || *a == 0) {
            return d == 0;
        }
        
        a++;
        b++;
    }
}

void findMusicCallback(const char *filename, void *userdata) {
    int len = strlen(filename);
    
    if (len < 5 || !caseInsensitiveStrEquals(&filename[len-4], ".s3m")) {
        return;
    }
    
    char *copy = malloc(sizeof(char) * (len + 1));
    strcpy(copy, filename);
    files[fileCount++] = copy;
    
    pd->system->logToConsole("file: %s", copy);
}

void redraw(void)
{
    int scroll = MAX(MIN(selection - 3, fileCount - 7), 0);
    int end = MIN(scroll + 7, fileCount);
    
    pd->graphics->clear(kColorWhite);
    pd->graphics->setFont(listFont);
    
    for(int i = scroll; i < end; ++i) {
        pd->graphics->drawText(files[i], strlen(files[i]), kUTF8Encoding, MARGINS, (i - scroll) * LINE_HEIGHT + MARGINS);
    }
    
    int n = selection - scroll;
    pd->graphics->fillRect(0, n * LINE_HEIGHT, 400, LINE_HEIGHT, kColorXOR);
    
    pd->graphics->setFont(infoFont);
    pd->graphics->drawLine(0, LINE_HEIGHT * 7, 400, LINE_HEIGHT * 7, 2, kColorBlack);
    pd->graphics->drawText(infoText, strlen(infoText), kUTF8Encoding, MARGINS, LINE_HEIGHT * 7 + MARGINS);
}

void loadSelection(void)
{
    char *path = NULL;
    
    if (currentMusicIndex != -1) {
        stopTrackerMusic();
        freeTrackerMusic(&currentMusic);
    }
    
    pd->system->logToConsole("Loading: %s", files[selection]);
    
    pd->system->formatString(&path, "music/%s", files[selection]);
    int error = loadMusicFromS3M(&currentMusic, path, kFileRead | kFileReadData);
    pd->system->realloc(path, 0);
    
    if (error != kMusicNoError) {
        pd->system->error("Failed to load music with error code: %d", error);
        return;
    }
    
    currentMusicIndex = selection;
}

void play(void)
{
    if (currentMusicIndex != selection) {
        loadSelection();
    }
    
    playTrackerMusic(&currentMusic, 0);
}

void stop(void)
{
    stopTrackerMusic();
}

void adjustSpeedFromCrank(void)
{
    float crank = 0.0f;
    
    crankValues[crankValueIndex] = pd->system->getCrankChange();
    crankValueIndex = (crankValueIndex + 1) % CRANK_SMOOTHING;
    
    // Apply a little bit of smoothing to the crank data to make it sound more
    // like we're messing with a record player:
    for(int i = 0; i < CRANK_SMOOTHING; ++i) {
        crank += crankValues[i];
    }
    
    crank /= CRANK_SMOOTHING;
    
    if (crank != 0.0f) {
        float d = crank * CRANK_SPEED * 0.01f;
        
        if (crank > 0.0f) {
            setTrackerMusicSpeed(d + 1.0f);
        } else {
            setTrackerMusicSpeed(1.0f / (-d + 1.0f));
        }
        
        setTrackerMusicPitchShift(d);
        
    } else {
        setTrackerMusicSpeed(1.0f);
        setTrackerMusicPitchShift(0.0f);
    }
}

void startup(void)
{
    const char *err = NULL;
    initializeTrackerMusic(pd);
    
    pd->file->listfiles("music", findMusicCallback, NULL, false);
    
    listFont = pd->graphics->loadFont("Nontendo/Nontendo-Bold-2x", &err);
    
    if (err) {
        pd->system->error("Failed to load font: %s", err);
    }
    
    infoFont = pd->graphics->loadFont("Nontendo/Nontendo-Light-2x", &err);
    
    if (err) {
        pd->system->error("Failed to load font: %s", err);
    }
    
    redraw();
}

void shutdown(void)
{
    stopTrackerMusic();
    
    for(int i = 0; i < fileCount; ++i) {
        free(files[i]);
    }
    
    if (currentMusicIndex != -1) {
        freeTrackerMusic(&currentMusic);
    }
}

int update(void* ud)
{
    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);
    
    if (pushed & kButtonUp) {
        selection = MAX(selection - 1, 0);
        redraw();
        
    } else if (pushed & kButtonDown) {
        selection = MIN(selection + 1, fileCount - 1);
        redraw();
        
    } else if (pushed & kButtonA) {
        play();
        
    } else if (pushed & kButtonB) {
        stop();
    }
    
    adjustSpeedFromCrank();
    processTrackerMusicCycle();
    
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg)
{
	switch(event) {
        case kEventInit:
            pd = playdate;
            playdate->display->setRefreshRate(30);
            playdate->system->setUpdateCallback(update, NULL);
            startup();
            break;
        case kEventTerminate:
            shutdown();
            break;
        default:
            break;
	}
	
	return 0;
}
