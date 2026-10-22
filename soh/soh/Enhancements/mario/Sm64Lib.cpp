#include "Sm64Lib.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Sm64 {

namespace {

Api sApi = {};
bool sLoaded = false;
bool sTried = false;
std::string sStatus = "Not loaded yet";
std::vector<uint8_t> sRom;
std::vector<uint8_t> sTexture;
void* sLib = nullptr;

std::atomic<bool> sAudioRunning = false;
std::thread sAudioThread;
SDL_AudioDeviceID sAudioDev = 0;
bool sAudioInitialized = false;
std::mutex sMutex;

#ifdef _WIN32
constexpr const char* kLibNames[] = { "sm64.dll", "libsm64.dll" };
#elif defined(__APPLE__)
constexpr const char* kLibNames[] = { "libsm64.dylib" };
#else
constexpr const char* kLibNames[] = { "libsm64.so" };
#endif

// File names we accept for the SM64 ROM, next to the game or in the working directory
constexpr const char* kRomNames[] = {
    "sm64.us.z64", "baserom.us.z64", "sm64.z64", "Super Mario 64 (USA).z64", "Super Mario 64 (U) [!].z64",
    "sm64.us.n64", "sm64.us.v64",    "sm64.n64", "sm64.v64",
};

void* OpenLib(const std::string& path) {
#ifdef _WIN32
    return (void*)LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* Sym(const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)sLib, name);
#else
    return dlsym(sLib, name);
#endif
}

template <typename T> bool Bind(T& fn, const char* name) {
    fn = reinterpret_cast<T>(Sym(name));
    if (fn == nullptr) {
        SPDLOG_ERROR("[Mario] libsm64 is missing symbol {}", name);
        return false;
    }
    return true;
}

bool LoadLib() {
    for (const char* name : kLibNames) {
        for (const std::string& path : { Ship::Context::GetPathRelativeToAppDirectory(name), std::string(name) }) {
            sLib = OpenLib(path);
            if (sLib != nullptr) {
                SPDLOG_INFO("[Mario] Loaded {}", path);
                break;
            }
        }
        if (sLib != nullptr) {
            break;
        }
    }
    if (sLib == nullptr) {
        sStatus = std::string("Could not find ") + kLibNames[0] + " next to the game";
        return false;
    }

    bool ok = true;
    ok &= Bind(sApi.global_init, "sm64_global_init");
    ok &= Bind(sApi.global_terminate, "sm64_global_terminate");
    ok &= Bind(sApi.audio_init, "sm64_audio_init");
    ok &= Bind(sApi.audio_tick, "sm64_audio_tick");
    ok &= Bind(sApi.static_surfaces_load, "sm64_static_surfaces_load");
    ok &= Bind(sApi.mario_create, "sm64_mario_create");
    ok &= Bind(sApi.mario_tick, "sm64_mario_tick");
    ok &= Bind(sApi.mario_delete, "sm64_mario_delete");
    ok &= Bind(sApi.set_mario_action, "sm64_set_mario_action");
    ok &= Bind(sApi.set_mario_position, "sm64_set_mario_position");
    ok &= Bind(sApi.set_mario_faceangle, "sm64_set_mario_faceangle");
    ok &= Bind(sApi.set_mario_velocity, "sm64_set_mario_velocity");
    ok &= Bind(sApi.set_mario_forward_velocity, "sm64_set_mario_forward_velocity");
    ok &= Bind(sApi.set_mario_water_level, "sm64_set_mario_water_level");
    ok &= Bind(sApi.set_mario_health, "sm64_set_mario_health");
    ok &= Bind(sApi.mario_take_damage, "sm64_mario_take_damage");
    ok &= Bind(sApi.surface_object_create, "sm64_surface_object_create");
    ok &= Bind(sApi.surface_object_move, "sm64_surface_object_move");
    ok &= Bind(sApi.surface_object_delete, "sm64_surface_object_delete");
    ok &= Bind(sApi.set_sound_volume, "sm64_set_sound_volume");
    if (!ok) {
        sStatus = "The libsm64 library is too old or damaged (missing functions)";
    }
    return ok;
}

// Converts byteswapped (.v64) and little endian (.n64) dumps to big endian (.z64)
bool NormalizeRomByteOrder(std::vector<uint8_t>& rom) {
    if (rom.size() < 0x40) {
        return false;
    }
    uint32_t magic = (rom[0] << 24) | (rom[1] << 16) | (rom[2] << 8) | rom[3];
    if (magic == 0x80371240) {
        return true;
    }
    if (magic == 0x37804012) { // v64: swap every 2 bytes
        for (size_t i = 0; i + 1 < rom.size(); i += 2) {
            std::swap(rom[i], rom[i + 1]);
        }
        return true;
    }
    if (magic == 0x40123780) { // n64: swap every 4 bytes
        for (size_t i = 0; i + 3 < rom.size(); i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
        return true;
    }
    return false;
}

bool LoadRom() {
    std::vector<std::string> candidates;
    for (const char* name : kRomNames) {
        candidates.push_back(Ship::Context::GetPathRelativeToAppDirectory(name));
        candidates.push_back(name);
    }

    for (const std::string& path : candidates) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            continue;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!NormalizeRomByteOrder(data)) {
            SPDLOG_WARN("[Mario] {} is not an N64 ROM", path);
            continue;
        }
        // Internal name at 0x20, region at 0x3E ('E' = USA). libsm64 only understands the US version.
        std::string internalName(reinterpret_cast<char*>(&data[0x20]), 14);
        if (internalName != "SUPER MARIO 64" || data[0x3E] != 'E' || data.size() != 8 * 1024 * 1024) {
            SPDLOG_WARN("[Mario] {} is not a US Super Mario 64 ROM", path);
            sStatus = "Found " + path + " but it is not the US version of Super Mario 64";
            continue;
        }
        sRom = std::move(data);
        SPDLOG_INFO("[Mario] Using SM64 ROM {}", path);
        return true;
    }

    if (sStatus.rfind("Found", 0) != 0) {
        sStatus = "Put your Super Mario 64 (USA) ROM next to the game, named sm64.us.z64";
    }
    return false;
}

void AudioThread() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    int16_t buffer[544 * 2 * 2];
    while (sAudioRunning) {
        uint32_t queued = SDL_GetQueuedAudioSize(sAudioDev) / 4;
        uint32_t numSamples;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            numSamples = sApi.audio_tick(queued, 1100, buffer);
        }
        if (queued < 6000) {
            SDL_QueueAudio(sAudioDev, buffer, numSamples * 2 * 4);
        }
        next += std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next);
    }
}

} // namespace

bool EnsureLoaded() {
    if (sLoaded) {
        return true;
    }
    if (sTried) {
        return false;
    }
    sTried = true;

    if (!LoadLib() || !LoadRom()) {
        SPDLOG_WARN("[Mario] {}", sStatus);
        return false;
    }

    sTexture.assign(4 * TEXTURE_WIDTH * TEXTURE_HEIGHT, 0);
    sApi.global_init(sRom.data(), sTexture.data());
    sLoaded = true;
    sStatus = "Mario is ready";
    return true;
}

void Reset() {
    StopAudio();
    if (sLoaded) {
        sApi.global_terminate();
    }
    sLoaded = false;
    sTried = false;
    sAudioInitialized = false;
}

bool IsLoaded() {
    return sLoaded;
}

const Api& Get() {
    return sApi;
}

std::mutex& Mutex() {
    return sMutex;
}

const uint8_t* Texture() {
    return sTexture.data();
}

const std::string& Status() {
    return sStatus;
}

void StartAudio() {
    if (!sLoaded || sAudioRunning) {
        return;
    }
    if (!sAudioInitialized) {
        sApi.audio_init(sRom.data());
        sAudioInitialized = true;
    }
    if (sAudioDev == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            SPDLOG_ERROR("[Mario] SDL audio init failed: {}", SDL_GetError());
            return;
        }
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 32000;
        want.format = AUDIO_S16;
        want.channels = 2;
        want.samples = 512;
        want.callback = nullptr;
        sAudioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (sAudioDev == 0) {
            SPDLOG_ERROR("[Mario] Could not open audio device: {}", SDL_GetError());
            return;
        }
    }
    SDL_PauseAudioDevice(sAudioDev, 0);
    sAudioRunning = true;
    sAudioThread = std::thread(AudioThread);
}

void StopAudio() {
    if (sAudioRunning) {
        sAudioRunning = false;
        if (sAudioThread.joinable()) {
            sAudioThread.join();
        }
    }
    if (sAudioDev != 0) {
        SDL_ClearQueuedAudio(sAudioDev);
        SDL_PauseAudioDevice(sAudioDev, 1);
    }
}

void SetVolume(float vol) {
    if (sLoaded) {
        sApi.set_sound_volume(vol);
    }
}

} // namespace Sm64
