#pragma once

// Runtime loader for libsm64 (https://github.com/libsm64/libsm64), which is Super Mario 64's
// Mario code from the SM64 decompilation (https://github.com/n64decomp/sm64) packaged as a library.
// It is loaded dynamically (sm64.dll / libsm64.so / libsm64.dylib next to the executable) so the
// game still runs normally when the library or the SM64 ROM is missing.

#include <cstdint>
#include <mutex>
#include <string>

extern "C" {

struct SM64Surface {
    int16_t type;
    int16_t force;
    uint16_t terrain;
    int32_t vertices[3][3];
};

struct SM64MarioInputs {
    float camLookX, camLookZ;
    float stickX, stickY;
    uint8_t buttonA, buttonB, buttonZ;
};

struct SM64ObjectTransform {
    float position[3];
    float eulerRotation[3];
};

struct SM64SurfaceObject {
    struct SM64ObjectTransform transform;
    uint32_t surfaceCount;
    struct SM64Surface* surfaces;
};

struct SM64MarioState {
    float position[3];
    float velocity[3];
    float faceAngle;
    float forwardVelocity;
    int16_t health;
    uint32_t action;
    int32_t animID;
    int16_t animFrame;
    uint32_t flags;
    uint32_t particleFlags;
    int16_t invincTimer;
};

struct SM64MarioGeometryBuffers {
    float* position;
    float* normal;
    float* color;
    float* uv;
    uint16_t numTrianglesUsed;
};
}

namespace Sm64 {

enum {
    TEXTURE_WIDTH = 64 * 11,
    TEXTURE_HEIGHT = 64,
    GEO_MAX_TRIANGLES = 1024,
};

// SM64 surface types / terrains we use
constexpr int16_t SURFACE_DEFAULT = 0x0000;
constexpr int16_t SURFACE_BURNING = 0x0001;
constexpr int16_t SURFACE_VERY_SLIPPERY = 0x0013;
constexpr int16_t SURFACE_SLIPPERY = 0x0014;
constexpr int16_t SURFACE_NOT_SLIPPERY = 0x0015;
constexpr int16_t SURFACE_SHALLOW_QUICKSAND = 0x0021;
constexpr uint16_t TERRAIN_GRASS = 0x0000;
constexpr uint16_t TERRAIN_STONE = 0x0001;
constexpr uint16_t TERRAIN_SNOW = 0x0002;
constexpr uint16_t TERRAIN_SAND = 0x0003;

// SM64 Mario actions we refer to
constexpr uint32_t ACT_IDLE = 0x0C400201;
constexpr uint32_t ACT_FREEFALL = 0x0100088C;
constexpr uint32_t ACT_GROUND_POUND = 0x008008A9;
constexpr uint32_t ACT_GROUND_POUND_LAND = 0x0080023C;
constexpr uint32_t ACT_DIVE = 0x0188088A;
constexpr uint32_t ACT_SLIDE_KICK = 0x018008AA;
constexpr uint32_t ACT_SLIDE_KICK_SLIDE = 0x0080045A;
constexpr uint32_t ACT_JUMP_KICK = 0x018008AC;
constexpr uint32_t ACT_FLAG_ATTACKING = 0x00800000;
constexpr uint32_t ACT_FLAG_AIR = 0x00000800;
constexpr uint32_t ACT_GROUP_MASK = 0x000001C0;
constexpr uint32_t ACT_GROUP_STATIONARY = 0x00000000;
constexpr uint32_t ACT_GROUP_MOVING = 0x00000040;
constexpr uint32_t ACT_GROUP_AIRBORNE = 0x00000080;
constexpr uint32_t ACT_GROUP_SUBMERGED = 0x000000C0;
constexpr uint32_t INT_SUBTYPE_BIG_KNOCKBACK = 0x00000008;
constexpr int16_t FULL_HEALTH = 0x0880;

struct Api {
    void (*global_init)(const uint8_t* rom, uint8_t* outTexture);
    void (*global_terminate)(void);
    void (*audio_init)(const uint8_t* rom);
    uint32_t (*audio_tick)(uint32_t numQueuedSamples, uint32_t numDesiredSamples, int16_t* audioBuffer);
    void (*static_surfaces_load)(const struct SM64Surface* surfaceArray, uint32_t numSurfaces);
    int32_t (*mario_create)(float x, float y, float z);
    void (*mario_tick)(int32_t marioId, const struct SM64MarioInputs* inputs, struct SM64MarioState* outState,
                       struct SM64MarioGeometryBuffers* outBuffers);
    void (*mario_delete)(int32_t marioId);
    void (*set_mario_action)(int32_t marioId, uint32_t action);
    void (*set_mario_position)(int32_t marioId, float x, float y, float z);
    void (*set_mario_faceangle)(int32_t marioId, float y);
    void (*set_mario_velocity)(int32_t marioId, float x, float y, float z);
    void (*set_mario_forward_velocity)(int32_t marioId, float vel);
    void (*set_mario_water_level)(int32_t marioId, signed int level);
    void (*set_mario_health)(int32_t marioId, uint16_t health);
    void (*mario_take_damage)(int32_t marioId, uint32_t damage, uint32_t subtype, float x, float y, float z);
    uint32_t (*surface_object_create)(const struct SM64SurfaceObject* surfaceObject);
    void (*surface_object_move)(uint32_t objectId, const struct SM64ObjectTransform* transform);
    void (*surface_object_delete)(uint32_t objectId);
    void (*set_sound_volume)(float vol);
};

// Loads the library and the SM64 ROM. Returns true once Mario is usable. Safe to call repeatedly;
// after a failure it does not retry until Reset() is called.
bool EnsureLoaded();
void Reset();
bool IsLoaded();
const Api& Get();
// Mario's texture atlas (RGBA8, TEXTURE_WIDTH x TEXTURE_HEIGHT)
const uint8_t* Texture();
// Human readable status shown in the menu
const std::string& Status();

// Guards libsm64 calls shared between the game thread and the audio thread
std::mutex& Mutex();

// Mario's voice and sound effects through a separate SDL audio stream
void StartAudio();
void StopAudio();
void SetVolume(float vol);

} // namespace Sm64
