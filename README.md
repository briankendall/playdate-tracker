# playdate-tracker

A tracker for Playdate that can play S3M files (and possibly some day other tracker formats as well). It's written in C using the Playdate PDSynth API. It can play complex 16+ channel music without running out of CPU cycles, and 4 channel music without breaking a sweat.

## Use

#### Compilation:

Include all the files in the `tracker_music` folder into your C project, and make sure your compiler can find the header files. If you're compiling with gcc or clang, you'll need to add the following compilation flags: `-fms-extensions` and `-Wno-microsoft-anon-tag` since the code makes use of anonymous structs.

The `CMakeLists.txt` file included in the demo app of this repo demonstrates how to do this.

#### Functions:

In your code, add the following include statements:

    #include "tracker_music.h"
    #include "s3m.h"

Before doing anything else, call this function to initialize the library by passing in a pointer to your `PlaydateAPI` instance:

    void initializeTrackerMusic(PlaydateAPI *inAPI);

Every cycle in your update function, call the following:

    void processTrackerMusicCycle();
    
Without this, no music will play. This function processes the next row of the currently playing music as needed.

To load an S3M file:

    int loadMusicFromS3M(TrackerMusic *music, char *path, FileOptions mode)

where `music` is a pointer to a `TrackerMusic` struct, and `path` is the path of the S3M file you want to load, and `mode` is the mode for opening the file, which must be at least one of: `kFileRead` or `kFileReadData`. It returns `kMusicNoError` is everything goes well, otherwise it'll return an error code and print some information to the console.

To play loaded music:

    void playTrackerMusic(TrackerMusic *music, uint32_t when);

where `music` is the already loaded music you want to play, and `when` is the sample time you want it to start playing. (Same as the `when` parameter used throughout the Playdate Sound API.)

To stop the currently playing music:

    void stopTrackerMusic();

To free up a `TrackerMusic` instance's resources when you're done with it:

    void freeTrackerMusic(TrackerMusic *music);

The following functions allow control of the currently playing music:

	void setTrackerMusicVolume(float vol);

Sets the volume, where `vol` is a value from 0.0 to 1.0.

	void setTrackerMusicPaused(bool paused);

Pauses or resumes the music. Note that any sustained notes will keep playing when the music is paused!

	void setTrackerMusicPosition(uint8_t orderIndex, uint8_t row);

Sets the next row that will be played when the music steps into a new row. `orderIndex` is which pattern position in the current module to play (that is, not the pattern index itself, but the index in the module's ordered list of patterns), and `row` is which row in that pattern to play.

    void getTrackerMusicPosition(uint8_t *orderIndex, uint8_t *row);
    
Returns the row and pattern index (i.e. the current index in the module's ordered list of patterns) of the last processed step of the music. Either argument can be NULL if you don't need that value. In fact both of them can be NULL if you feel like wasting a few CPU cycles.

	void setTrackerMusicSpeed(float speed);

Sets playback speed, where `speed` is a factor multiplied by the current play rate. So a `speed` of 2.0 doubles the playback speed, and 0.5 halves it.

	void setTrackerMusicPitchShift(float pitch);

Pitch shifts the entire playing music. A value of 0.0 is no pitch shift, 1.0 shifts everything up one octave, 2.0 shifts everything up two octaves, -1.0 shifts everything down one octave, and so on. (i.e. it works the same as the return value of a `PDSynth` frequency modulator.)

#### Preprocessor Macros

You can define the macro `TRACKER_MUSIC_MAX_CHANNELS` ahead of time (such as in your `CMakeLists.txt`) and set its value to the maximum number of channels of any of the music you're going to play if you know that's going to be less than 32 channels, in order to save a bit of memory and CPU cycles.

You can set `TRACKER_MUSIC_VERBOSE` to 1 if you want to get lots of console logging when playing music.

This library makes use of a macro `PLAYDATE_API_VERSION` for checking the Playdate API version and including bug workarounds as needed. If this macro is not defined then all workarounds are used. This macro should correspond to the API version as five or six digit integer in the form AABBCC, where each set of two digits refers to the major, minor and patch version number respectively. So API version 2.5.0 (the current version as of writing this) would be `20500`. (Note: not `020500`, as the C compiler would interpret that as an octal rather than decimal number!)

The `CMakeLists.txt` included with the demo program in this repo shows demonstrates how to automatically define `PLAYDATE_API_VERSION` for the current Playdate API version.

## Why S3M and not IT or XM?

The multi-platform game project I'm working on requires me to be able to play tracker music that has the capabilities of a MOD file, because one of the platforms I'm supporting can't play anything more advanced than MOD. However, MOD files don't allow for high quality samples, so I decided to go for S3M on the Playdate since it's functionally equivalent to a MOD file, except with the ability to set the sample rate on each sample. (It can also use more channels and has a few more effects.) S3M also lines up well with the capabilities of `PDSynth`. Implementing an XM or IT player would require being able to manually implement instrument envelopes, which is totally doable but outside the scope of what I need.

That said, I attempted to code this library in such a way as to make it not too difficult to expand its capabilities to allow other tracker formats. It wouldn't take much to get it to play MOD files, and XM or IT files would require implementing instruments and their envelopes (as mentioned), variable length patterns, linear frequency slides, plus a few new effects.

### How compatible is it?

Only sample instruments are supported. Adlib instruments are not supported (as that would require emulating an Yamaha OPL2 chip which is well outside the scope of this project). Also, currently not every S3M effect is implemented. Here's a list of the ones that are _not_ implemented:

- `M` Set Channel Volume
- `N` Channel Volume Slide
- `W` Global Volume Slide
- `Y` Panbrello
- `Z` MIDI Macro
- `S1` Glissando Control 
- `S2` Set Finetune
- `S5` Set Panbrello Waveform 
- `S6` Fine Pattern Delay 
- `S9` Sound Control 
- `SA` High Offset
- `SB` Pattern Loop
- `SC` Note Cut
- `SE` Pattern Delay

The good news is that these effects are rare and/or not well supported, and most music doesn't make use of them. All of the S3M music I've downloaded to test out this player didn't make use of these effects. It also probably wouldn't be too hard to implement any one of these if they're truly needed.

## Demo program

This library comes with a little demo S3M player to show how to use the library, and let you have some fun changing the playback speed of the music using the Playdate's crank like you were messing with an old turntable or cassette deck.

To build it with CMake using make in a bash-like environment:

    cd path/to/playdate-tracker/demo
    cmake --preset playdate-emulator .
    cd build/playdate-emulator
    make
    
Or to build a version for the Playdate console, replace `playdate-emulator` with `playdate` on both the second and third line.

It comes with a selection of music built in that I picked from the top rated S3M files from [Nectarine Demo Scene radio](https://scenestream.net) and [The Mod Archive](https://modarchive.org/). Also, a few of the worst rated. Plus a couple of favorites of mine.

The music is:

* `2ND_PM.s3m`: UnreaL II by Purple Motion
* `2nd_reality.s3m`: The 2nd Reality by Skaven
* `blah_blob.s3m`: The Blah Blob background by me (Brian Kendall)
* `celestial_fantasia.s3m`: Celestial Fantasia by BeaT
* `chrono_trigger_forest.s3m`: A rendition of Secret of the Forest from the Chrono Trigger soundtrack
* `chronologie_part_4.s3m`: Chronologie - Part 4 by Karsten Koch
* `frog_dance.s3m`: Frog Dance by Krotan
* `icefront.s3m`: Ice Frontier by Skaven
* `inside_out.s3m`: Insideout by Amadeus Voxon
* `mech8.S3M`: mechanism eight by Necros
* `organic.s3m`: Organic by siren
* `Rakkautta_Vain.s3m`: Rakkautta Vain by Chanel5, Fornicator, and SineWave 
* `starshine.s3m`: Starshine by Purple Motion
* `world_of_plastic.s3m`: World of Plastic by Purple Motion

You can also load up your own music by putting your Playdate in data disk mode, and then going to the folder: `/Data/net.briankendall.PlaydateTracker/music`, creating folders as needed, and copying your S3M files into that music folder.
