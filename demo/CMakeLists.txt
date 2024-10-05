cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})

if (NOT ${ENVSDK} STREQUAL "")
	# Convert path from Windows
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
			COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
			COMMAND head -n 1
			COMMAND cut -c9-
			OUTPUT_VARIABLE SDK
			OUTPUT_STRIP_TRAILING_WHITESPACE
	)
endif()

if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK Path not found; set ENV value PLAYDATE_SDK_PATH")
	return()
endif()

project(PlaydateTracker C ASM)

# The following reads the Playdate API version number at the root of the API
# directory into a variable and adds a precompiler definition in the form of 5-6
# digit decimal integer, where the first two digits correspond to the major
# version (or more likely the first digit if the major version is a single digit
# number), the next two the minor version, and the final two the patch version.
# This is used to conditionally include certain workarounds for bugs in earlier
# versions of the Playdate API.
if(EXISTS "${SDK}/VERSION.txt")
    file(READ "${SDK}/VERSION.txt" VERSION_CONTENTS)
    string(STRIP "${VERSION_CONTENTS}" VERSION_NUMBER)
    string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ ${VERSION_NUMBER})
    math(EXPR VERSION_INTEGER "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")
    add_compile_definitions(PLAYDATE_API_VERSION=${VERSION_INTEGER})
endif()

add_compile_definitions(TRACKER_MUSIC_VERBOSE=1 TRACKER_MUSIC_MAX_CHANNELS=32)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(CMAKE_XCODE_GENERATE_SCHEME TRUE)

# Game Name Customization
set(PLAYDATE_GAME_NAME PlaydateTracker)
set(PLAYDATE_GAME_DEVICE PlaydateTracker_DEVICE)

# Required to avoid warnings about anonymous structs when compiling with gcc or clang:
if (NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fms-extensions -Wno-microsoft-anon-tag")
endif()

set(SOURCES
    main.c
    ../tracker_music/tracker_music.c
    ../tracker_music/s3m.c
)

set(PLAYDATE_PDX_DIR "${CMAKE_BINARY_DIR}")

if (TOOLCHAIN STREQUAL "armgcc")
    set(PLAYDATE_GAME_TARGET ${PLAYDATE_GAME_DEVICE})
	add_executable(${PLAYDATE_GAME_DEVICE} ${SOURCES})
    
    add_custom_target(AfterPDXCreation ALL)
    add_dependencies(AfterPDXCreation ${PLAYDATE_GAME_TARGET})
	add_custom_command(
		TARGET AfterPDXCreation POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f ${PLAYDATE_PDX_DIR}/${PLAYDATE_GAME_NAME}.pdx/pdex.dylib
	)
else()
    set(PLAYDATE_GAME_TARGET ${PLAYDATE_GAME_NAME})
	add_library(${PLAYDATE_GAME_NAME} SHARED ${SOURCES})
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/playdate_game.cmake)

target_include_directories(${PLAYDATE_GAME_TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(${PLAYDATE_GAME_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/../tracker_music)
