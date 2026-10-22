// Mario Replaces Link
//
// Puts Super Mario 64's Mario, with his full moveset and physics, into Ocarina of Time in place of Link.
// Mario's code comes from libsm64 (built from the SM64 decompilation) and his model, animations and
// voice are read from the player's own SM64 ROM at runtime.
//
// How the two games share one player:
// * OoT's scene collision, moving platforms, doors and water are fed to Mario's physics.
// * While Link would be standing, walking, running, falling or swimming, Mario is in control: every
//   frame Link's actor is moved to wherever Mario's physics put him, and Link's own logic keeps running
//   with neutral input so loading zones, voids, enemy hits, NPC prompts and item buttons keep working.
// * Context actions (talking, doors, chests, items, ladders, crawlspaces, cutscenes, horse riding) are
//   handed to Link's logic. Mario follows Link's position and animates along until Link is free again.
// * Link's model is never drawn; Mario is drawn from libsm64's output mesh.

#include "Sm64Lib.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/frame_interpolation.h"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "regs.h"
extern PlayState* gPlayState;
}

#define CVAR_MARIO(name) CVAR_ENHANCEMENT("Mario." name)
#define CVAR_MARIO_ENABLED_NAME CVAR_MARIO("Enabled")
#define CVAR_MARIO_ENABLED_VALUE CVarGetInteger(CVAR_MARIO_ENABLED_NAME, 0)

namespace {

enum class Mode {
    Mario, // Mario's physics drive the player
    Link,  // Link's logic drives the player, Mario follows
};

struct DynaSurface {
    uint32_t objectId;
    Actor* actor;
    CollisionHeader* colHeader;
    Vec3f scale;
    bool seen;
};

struct DoorBlocker {
    uint32_t objectId;
    bool seen;
};

struct State {
    // scene
    PlayState* play = nullptr;
    CollisionHeader* colHeader = nullptr;
    float scale = 3.0f; // SM64 units per OoT unit
    bool sceneReady = false;
    std::unordered_map<int, DynaSurface> dyna; // keyed by bg actor index
    std::unordered_map<Actor*, DoorBlocker> doors;

    // mario
    int32_t marioId = -1;
    SM64MarioState state = {};
    SM64MarioGeometryBuffers geo = {};
    float tickAccumulator = 0.0f;
    Mode mode = Mode::Mario;
    bool handedToLink = false; // this frame we passed a button or the stick to Link on purpose
    Vec3f lastSetPos = {};
    int32_t healthRemainder = 0;
    bool pendingPostUpdate = false;

    // attacking
    ColliderCylinder attack;
    bool attackInit = false;
    PlayState* attackPlay = nullptr;
};

State s;

std::vector<float> sPositions(9 * Sm64::GEO_MAX_TRIANGLES);
std::vector<float> sNormals(9 * Sm64::GEO_MAX_TRIANGLES);
std::vector<float> sColors(9 * Sm64::GEO_MAX_TRIANGLES);
std::vector<float> sUvs(6 * Sm64::GEO_MAX_TRIANGLES);

// Mario's texture atlas split into its eleven 64x64 textures, as RGBA32 for the renderer
constexpr int kNumTextures = 11;
alignas(16) static uint8_t sTextureChunks[kNumTextures][64 * 64 * 4];

const Sm64::Api& lib() {
    return Sm64::Get();
}

bool IsMarioActive() {
    return s.marioId >= 0 && Sm64::IsLoaded();
}

// ---------------------------------------------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------------------------------------------

float BaseScale() {
    // Adult Link is about 56 units tall and child Link about 40. Mario is 160 SM64 units tall. These pick a
    // Mario a little shorter than Link at both ages.
    float base = LINK_IS_ADULT ? 3.2f : 4.0f;
    float size = CVarGetFloat(CVAR_MARIO("Size"), 1.0f);
    if (size < 0.25f) {
        size = 0.25f;
    }
    return base / size;
}

void SurfaceTypeFor(CollisionContext* colCtx, CollisionPoly* poly, s32 bgId, int16_t* type, uint16_t* terrain) {
    *type = Sm64::SURFACE_DEFAULT;
    *terrain = Sm64::TERRAIN_STONE;

    u32 floorType = SurfaceType_GetFloorType(colCtx, poly, bgId);
    u32 floorEffect = SurfaceType_GetFloorEffect(colCtx, poly, bgId);
    u16 sfx = SurfaceType_GetSfx(colCtx, poly, bgId);

    if (floorType == 5) {
        *type = Sm64::SURFACE_VERY_SLIPPERY; // ice
        *terrain = Sm64::TERRAIN_SNOW;
    } else if (floorType == 2 || floorType == 3) {
        *type = Sm64::SURFACE_BURNING; // hot floor
    } else if (floorType == 4) {
        *type = Sm64::SURFACE_SHALLOW_QUICKSAND; // sinking sand
        *terrain = Sm64::TERRAIN_SAND;
    } else if (floorEffect == 1) {
        *type = Sm64::SURFACE_SLIPPERY; // steep slope Link slides down
    }

    switch (sfx) {
        case 0: // dirt
        case 8: // grass
            *terrain = Sm64::TERRAIN_GRASS;
            break;
        case 1: // sand
            *terrain = Sm64::TERRAIN_SAND;
            break;
        case 14: // ice
            *terrain = Sm64::TERRAIN_SNOW;
            break;
        default:
            break;
    }
}

// Converts one OoT collision triangle (in the given vertex list) to an SM64 surface.
bool ConvertPoly(CollisionContext* colCtx, CollisionPoly* poly, s32 bgId, Vec3s* vtxList, const Vec3f& vtxScale,
                 SM64Surface* out) {
    if (SurfaceType_IsIgnoredByEntities(colCtx, poly, bgId)) {
        return false;
    }

    Vec3s* v[3] = {
        &vtxList[COLPOLY_VTX_INDEX(poly->flags_vIA)],
        &vtxList[COLPOLY_VTX_INDEX(poly->flags_vIB)],
        &vtxList[poly->vIC],
    };

    float p[3][3];
    for (int i = 0; i < 3; i++) {
        p[i][0] = v[i]->x * vtxScale.x * s.scale;
        p[i][1] = v[i]->y * vtxScale.y * s.scale;
        p[i][2] = v[i]->z * vtxScale.z * s.scale;
    }

    // Keep the winding that agrees with OoT's stored normal, SM64 derives the normal from the winding
    float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
    float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float dot = nx * poly->normal.x + ny * poly->normal.y + nz * poly->normal.z;
    if (std::fabs(nx) + std::fabs(ny) + std::fabs(nz) < 0.0001f) {
        return false; // degenerate
    }
    int order[3] = { 0, 1, 2 };
    if (dot < 0.0f) {
        order[1] = 2;
        order[2] = 1;
    }

    SurfaceTypeFor(colCtx, poly, bgId, &out->type, &out->terrain);
    out->force = 0;
    for (int i = 0; i < 3; i++) {
        out->vertices[i][0] = (int32_t)lrintf(p[order[i]][0]);
        out->vertices[i][1] = (int32_t)lrintf(p[order[i]][1]);
        out->vertices[i][2] = (int32_t)lrintf(p[order[i]][2]);
    }
    return true;
}

void LoadStaticCollision(PlayState* play) {
    CollisionContext* colCtx = &play->colCtx;
    CollisionHeader* header = colCtx->colHeader;
    std::vector<SM64Surface> surfaces;
    surfaces.reserve(header->numPolygons);
    Vec3f one = { 1.0f, 1.0f, 1.0f };

    for (s32 i = 0; i < header->numPolygons; i++) {
        SM64Surface surf;
        if (ConvertPoly(colCtx, &header->polyList[i], BGCHECK_SCENE, header->vtxList, one, &surf)) {
            surfaces.push_back(surf);
        }
    }

    lib().static_surfaces_load(surfaces.data(), (uint32_t)surfaces.size());
}

SM64ObjectTransform TransformFor(const ScaleRotPos& t) {
    SM64ObjectTransform out;
    out.position[0] = t.pos.x * s.scale;
    out.position[1] = t.pos.y * s.scale;
    out.position[2] = t.pos.z * s.scale;
    // libsm64 takes degrees and rotates Z, then X, then Y like OoT's dynapoly does
    out.eulerRotation[0] = t.rot.x * (360.0f / 65536.0f);
    out.eulerRotation[1] = t.rot.y * (360.0f / 65536.0f);
    out.eulerRotation[2] = t.rot.z * (360.0f / 65536.0f);
    return out;
}

void DeleteDyna(int index) {
    auto it = s.dyna.find(index);
    if (it != s.dyna.end()) {
        lib().surface_object_delete(it->second.objectId);
        s.dyna.erase(it);
    }
}

// Mirrors OoT's moving collision (platforms, blocks, gates, bridges, elevators) into Mario's world
void UpdateDynamicCollision(PlayState* play) {
    DynaCollisionContext* dyna = &play->colCtx.dyna;
    Player* player = GET_PLAYER(play);

    for (auto& [index, entry] : s.dyna) {
        entry.seen = false;
    }

    for (int i = 0; i < BG_ACTOR_MAX; i++) {
        if (!(dyna->bgActorFlags[i] & 1)) {
            continue; // not in use
        }
        BgActor* bg = &dyna->bgActors[i];
        if (bg->actor == nullptr || bg->colHeader == nullptr || bg->actor == &player->actor) {
            continue;
        }
        if (dyna->bgActorFlags[i] & 2) {
            continue; // collision turned off
        }

        auto it = s.dyna.find(i);
        bool changed = it == s.dyna.end() || it->second.actor != bg->actor || it->second.colHeader != bg->colHeader ||
                       it->second.scale.x != bg->curTransform.scale.x ||
                       it->second.scale.y != bg->curTransform.scale.y ||
                       it->second.scale.z != bg->curTransform.scale.z;

        if (changed) {
            DeleteDyna(i);

            CollisionHeader* header = bg->colHeader;
            std::vector<SM64Surface> surfaces;
            surfaces.reserve(header->numPolygons);
            for (s32 p = 0; p < header->numPolygons; p++) {
                SM64Surface surf;
                if (ConvertPoly(&play->colCtx, &header->polyList[p], i, header->vtxList, bg->curTransform.scale,
                                &surf)) {
                    surfaces.push_back(surf);
                }
            }
            if (surfaces.empty()) {
                continue;
            }

            SM64SurfaceObject obj;
            obj.transform = TransformFor(bg->curTransform);
            obj.surfaceCount = (uint32_t)surfaces.size();
            obj.surfaces = surfaces.data();

            DynaSurface entry;
            entry.objectId = lib().surface_object_create(&obj);
            entry.actor = bg->actor;
            entry.colHeader = bg->colHeader;
            entry.scale = bg->curTransform.scale;
            entry.seen = true;
            s.dyna[i] = entry;
        } else {
            SM64ObjectTransform t = TransformFor(bg->curTransform);
            lib().surface_object_move(it->second.objectId, &t);
            it->second.seen = true;
        }
    }

    std::vector<int> gone;
    for (auto& [index, entry] : s.dyna) {
        if (!entry.seen) {
            gone.push_back(index);
        }
    }
    for (int index : gone) {
        DeleteDyna(index);
    }
}

// Ordinary wooden doors have no collision in OoT; Link simply never walks through them. Mario would, and end up in
// an unloaded room, so every door gets a thin solid slab until Link's own door logic opens it.
void UpdateDoorBlockers(PlayState* play) {
    for (auto& [actor, entry] : s.doors) {
        entry.seen = false;
    }

    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_DOOR].head; actor != nullptr; actor = actor->next) {
        bool hasDyna = false;
        for (auto& [index, entry] : s.dyna) {
            if (entry.actor == actor) {
                hasDyna = true;
                break;
            }
        }
        if (hasDyna) {
            continue;
        }

        auto it = s.doors.find(actor);
        if (it == s.doors.end()) {
            // A box 80 wide, 110 tall and 12 thick (OoT units), facing the door's direction
            const float hw = 40.0f * s.scale, h = 110.0f * s.scale, ht = 6.0f * s.scale;
            const float c[8][3] = {
                { -hw, 0, -ht }, { hw, 0, -ht }, { hw, h, -ht }, { -hw, h, -ht },
                { -hw, 0, ht },  { hw, 0, ht },  { hw, h, ht },  { -hw, h, ht },
            };
            // Two triangles per face, wound so normals point out of the box
            const int faces[12][3] = {
                { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 }, { 0, 4, 7 }, { 0, 7, 3 },
                { 1, 2, 6 }, { 1, 6, 5 }, { 3, 7, 6 }, { 3, 6, 2 }, { 0, 1, 5 }, { 0, 5, 4 },
            };
            SM64Surface surfaces[12];
            for (int f = 0; f < 12; f++) {
                surfaces[f].type = Sm64::SURFACE_NOT_SLIPPERY;
                surfaces[f].force = 0;
                surfaces[f].terrain = Sm64::TERRAIN_STONE;
                for (int k = 0; k < 3; k++) {
                    surfaces[f].vertices[k][0] = (int32_t)c[faces[f][k]][0];
                    surfaces[f].vertices[k][1] = (int32_t)c[faces[f][k]][1];
                    surfaces[f].vertices[k][2] = (int32_t)c[faces[f][k]][2];
                }
            }
            SM64SurfaceObject obj;
            obj.transform.position[0] = actor->world.pos.x * s.scale;
            obj.transform.position[1] = actor->world.pos.y * s.scale;
            obj.transform.position[2] = actor->world.pos.z * s.scale;
            obj.transform.eulerRotation[0] = 0.0f;
            obj.transform.eulerRotation[1] = actor->shape.rot.y * (360.0f / 65536.0f);
            obj.transform.eulerRotation[2] = 0.0f;
            obj.surfaceCount = 12;
            obj.surfaces = surfaces;
            s.doors[actor] = { lib().surface_object_create(&obj), true };
        } else {
            it->second.seen = true;
        }
    }

    for (auto it = s.doors.begin(); it != s.doors.end();) {
        if (!it->second.seen) {
            lib().surface_object_delete(it->second.objectId);
            it = s.doors.erase(it);
        } else {
            ++it;
        }
    }
}

void ClearDynamic() {
    for (auto& [index, entry] : s.dyna) {
        lib().surface_object_delete(entry.objectId);
    }
    s.dyna.clear();
    for (auto& [actor, entry] : s.doors) {
        lib().surface_object_delete(entry.objectId);
    }
    s.doors.clear();
}

void DeleteMario() {
    if (s.marioId >= 0 && Sm64::IsLoaded()) {
        std::lock_guard<std::mutex> lock(Sm64::Mutex());
        lib().mario_delete(s.marioId);
    }
    s.marioId = -1;
}

void Teardown() {
    DeleteMario();
    if (Sm64::IsLoaded()) {
        std::lock_guard<std::mutex> lock(Sm64::Mutex());
        ClearDynamic();
    }
    s.dyna.clear();
    s.doors.clear();
    s.sceneReady = false;
    s.play = nullptr;
    s.colHeader = nullptr;
    s.attackInit = false;
}

void SyncMarioToLink(Player* player) {
    lib().set_mario_position(s.marioId, player->actor.world.pos.x * s.scale, player->actor.world.pos.y * s.scale,
                             player->actor.world.pos.z * s.scale);
    lib().set_mario_faceangle(s.marioId, player->actor.shape.rot.y * (float)M_PI / 32768.0f);
}

bool EnsureScene(PlayState* play, Player* player) {
    if (!Sm64::EnsureLoaded()) {
        return false;
    }

    if (s.play != play || s.colHeader != play->colCtx.colHeader || s.scale != BaseScale()) {
        Teardown();
        s.play = play;
        s.colHeader = play->colCtx.colHeader;
        s.scale = BaseScale();

        std::lock_guard<std::mutex> lock(Sm64::Mutex());
        LoadStaticCollision(play);
        UpdateDynamicCollision(play);
        UpdateDoorBlockers(play);
        s.sceneReady = true;

        // Split the atlas into 64x64 textures once
        static bool texturesReady = false;
        if (!texturesReady) {
            const uint8_t* atlas = Sm64::Texture();
            for (int t = 0; t < kNumTextures; t++) {
                for (int y = 0; y < 64; y++) {
                    memcpy(&sTextureChunks[t][y * 64 * 4], &atlas[(y * Sm64::TEXTURE_WIDTH + t * 64) * 4], 64 * 4);
                }
            }
            texturesReady = true;
        }

        s.geo.position = sPositions.data();
        s.geo.normal = sNormals.data();
        s.geo.color = sColors.data();
        s.geo.uv = sUvs.data();
        s.geo.numTrianglesUsed = 0;
        s.mode = Mode::Link; // re-enter Mario mode once Link is free
        s.tickAccumulator = 0.0f;
        s.healthRemainder = 0;
    }

    if (s.marioId < 0) {
        std::lock_guard<std::mutex> lock(Sm64::Mutex());
        // Mario can only be created above a floor; retry every frame until he can be
        s.marioId = lib().mario_create(player->actor.world.pos.x * s.scale, player->actor.world.pos.y * s.scale + 2.0f,
                                       player->actor.world.pos.z * s.scale);
        if (s.marioId >= 0) {
            SyncMarioToLink(player);
            lib().set_mario_health(s.marioId, Sm64::FULL_HEALTH);
            s.lastSetPos = player->actor.world.pos;
            if (CVarGetInteger(CVAR_MARIO("Audio"), 1)) {
                Sm64::StartAudio();
            }
        }
    }

    return s.marioId >= 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Ticking Mario
// ---------------------------------------------------------------------------------------------------------------

int TicksThisFrame() {
    // OoT's logic runs at 20 frames per second (R_UPDATE_RATE 3), SM64 at 30
    float rate = R_UPDATE_RATE > 0 ? R_UPDATE_RATE : 3;
    s.tickAccumulator += 30.0f * rate / 60.0f;
    int ticks = (int)s.tickAccumulator;
    s.tickAccumulator -= ticks;
    return ticks > 3 ? 3 : ticks;
}

void UpdateWaterLevel(PlayState* play, Player* player) {
    f32 ySurface;
    WaterBox* waterBox;
    int level = -1000000;
    float x = s.state.position[0] / s.scale, z = s.state.position[2] / s.scale;
    // With the Iron Boots on Mario walks along the bottom like Link does
    if (player->currentBoots != PLAYER_BOOTS_IRON &&
        WaterBox_GetSurface1(play, &play->colCtx, x, z, &ySurface, &waterBox)) {
        level = (int)(ySurface * s.scale);
    }
    lib().set_mario_water_level(s.marioId, level);
}

void Tick(int count, const SM64MarioInputs& inputs) {
    for (int i = 0; i < count; i++) {
        lib().mario_tick(s.marioId, &inputs, &s.state, &s.geo);
    }
}

// Turns any health Mario lost on his own (falls, lava, running out of air) into OoT damage, then refills him.
// Link's hearts are the only health bar.
void ConvertMarioHealth(PlayState* play) {
    int lost = Sm64::FULL_HEALTH - s.state.health;
    if (lost > 0) {
        s.healthRemainder += lost;
        // One of Mario's eight wedges (0x100) is a quarter heart (4 OoT health units)
        int units = s.healthRemainder / 0x40;
        s.healthRemainder -= units * 0x40;
        if (units > 0 && gSaveContext.health > 0) {
            Health_ChangeBy(play, -units);
        }
    }
    lib().set_mario_health(s.marioId, Sm64::FULL_HEALTH);
}

void ApplyMarioToLink(Player* player) {
    player->actor.world.pos.x = s.state.position[0] / s.scale;
    player->actor.world.pos.y = s.state.position[1] / s.scale;
    player->actor.world.pos.z = s.state.position[2] / s.scale;

    s16 yaw = (s16)(s.state.faceAngle * 32768.0f / (float)M_PI);
    player->actor.shape.rot.y = yaw;
    player->actor.world.rot.y = yaw;
    player->yaw = yaw;

    player->actor.velocity.x = player->actor.velocity.y = player->actor.velocity.z = 0.0f;
    player->actor.speedXZ = 0.0f;
    player->linearVelocity = 0.0f;

    // Mario takes his own fall damage; don't let Link take it too
    player->fallStartHeight = (s16)player->actor.world.pos.y;
    player->fallDistance = 0;

    s.lastSetPos = player->actor.world.pos;
}

// Buttons whose prompt on the A button means "let Link do this"
bool IsLinkDoAction(u16 doAction) {
    switch (doAction) {
        case DO_ACTION_CHECK:
        case DO_ACTION_ENTER:
        case DO_ACTION_OPEN:
        case DO_ACTION_SPEAK:
        case DO_ACTION_GRAB:
        case DO_ACTION_THROW:
        case DO_ACTION_DROP:
        case DO_ACTION_PUTAWAY:
        case DO_ACTION_DECIDE:
            return true;
        default:
            return false;
    }
}

// Ladders, vines and crawlspaces need Link's logic to start. When Mario pushes into one, the stick is passed to Link.
bool WantsLinkClimbAssist(PlayState* play, Player* player, const Input& raw) {
    if (player->actor.wallPoly == nullptr || !(player->actor.bgCheckFlags & BGCHECKFLAG_WALL)) {
        return false;
    }
    uint32_t group = s.state.action & Sm64::ACT_GROUP_MASK;
    if (group != Sm64::ACT_GROUP_STATIONARY && group != Sm64::ACT_GROUP_MOVING) {
        return false;
    }
    int mag = raw.cur.stick_x * raw.cur.stick_x + raw.cur.stick_y * raw.cur.stick_y;
    if (mag < 40 * 40) {
        return false;
    }
    s32 flags = SurfaceType_GetWallFlags(&play->colCtx, player->actor.wallPoly, player->actor.wallBgId);
    if (!(flags & (WALL_FLAG_CLIMBABLE | WALL_FLAG_LADDER | WALL_FLAG_CRAWLSPACE))) {
        return false;
    }
    s16 towardWall = player->actor.wallYaw + 0x8000;
    s16 diff = towardWall - player->actor.shape.rot.y;
    return ABS(diff) < 0x3000;
}

SM64MarioInputs InputsFromController(PlayState* play, const Input& raw, bool giveA) {
    SM64MarioInputs in = {};
    Camera* cam = GET_ACTIVE_CAM(play);
    in.camLookX = cam->at.x - cam->eye.x;
    in.camLookZ = cam->at.z - cam->eye.z;
    in.stickX = raw.cur.stick_x / 80.0f;
    in.stickY = -raw.cur.stick_y / 80.0f;
    float mag = sqrtf(in.stickX * in.stickX + in.stickY * in.stickY);
    if (mag > 1.0f) {
        in.stickX /= mag;
        in.stickY /= mag;
    }
    in.buttonA = giveA && CHECK_BTN_ALL(raw.cur.button, BTN_A);
    in.buttonB = CHECK_BTN_ALL(raw.cur.button, BTN_B);
    in.buttonZ = CHECK_BTN_ANY(raw.cur.button, BTN_Z | BTN_R);
    return in;
}

// While Link is busy, Mario plays along: he walks when Link moves and stands when Link stands
void TickPuppet(PlayState* play, Player* player) {
    bool grounded = player->actor.bgCheckFlags & BGCHECKFLAG_GROUND;
    Vec3f delta;
    Math_Vec3f_Diff(&player->actor.world.pos, &s.lastSetPos, &delta);
    float speed = sqrtf(delta.x * delta.x + delta.z * delta.z);

    SyncMarioToLink(player);
    s.lastSetPos = player->actor.world.pos;

    if (!grounded) {
        return; // on a ladder, hanging, riding: keep Mario's last pose
    }

    if ((s.state.action & Sm64::ACT_GROUP_MASK) == Sm64::ACT_GROUP_AIRBORNE) {
        lib().set_mario_action(s.marioId, Sm64::ACT_IDLE);
    }

    SM64MarioInputs in = {};
    if (speed > 0.5f) {
        in.camLookX = delta.x;
        in.camLookZ = delta.z;
        float stick = speed / 6.0f;
        in.stickY = -(stick > 1.0f ? 1.0f : stick);
    } else {
        in.camLookZ = 1.0f;
    }
    Tick(TicksThisFrame(), in);
    lib().set_mario_health(s.marioId, Sm64::FULL_HEALTH);
}

void EnterMarioMode(Player* player) {
    s.mode = Mode::Mario;
    SyncMarioToLink(player);
    bool grounded = player->actor.bgCheckFlags & BGCHECKFLAG_GROUND;
    lib().set_mario_action(s.marioId, grounded ? Sm64::ACT_IDLE : Sm64::ACT_FREEFALL);
    lib().set_mario_velocity(s.marioId, 0.0f, 0.0f, 0.0f);
    lib().set_mario_forward_velocity(s.marioId, 0.0f);
    s.lastSetPos = player->actor.world.pos;
}

bool LinkIsBusy(PlayState* play, Player* player) {
    return (player->stateFlags1 & (PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_LOADING | PLAYER_STATE1_ON_HORSE |
                                   PLAYER_STATE1_TALKING | PLAYER_STATE1_CLIMBING_LADDER)) ||
           player->csAction != 0 || Player_InCsMode(play) || player->actor.parent != nullptr;
}

// Runs at the start of Link's update, before his logic. Decides who drives, ticks Mario and rewrites Link's input.
void OnPlayerUpdateCommon(Player* player, Input* input) {
    PlayState* play = gPlayState;
    s.pendingPostUpdate = false;

    if (!EnsureScene(play, player)) {
        return;
    }

    std::lock_guard<std::mutex> lock(Sm64::Mutex());

    UpdateDynamicCollision(play);
    UpdateDoorBlockers(play);

    // Something other than Mario moved Link since last frame (pushed by an NPC, respawned, warped): follow it
    if (Math_Vec3f_DistXYZ(&player->actor.world.pos, &s.lastSetPos) > 0.01f && s.mode == Mode::Mario) {
        SyncMarioToLink(player);
    }

    s32 category = Player_GetMarioActionCategory(player);
    bool consented = s.handedToLink;
    s.handedToLink = false;

    if (s.mode == Mode::Mario) {
        if (category == PLAYER_MARIO_ACTION_DAMAGE) {
            // Link was hit: Mario does his own knockback, away from where Link would have been pushed from
            float yaw = player->actor.world.rot.y * (float)M_PI / 32768.0f;
            float src[3] = { s.state.position[0] - sinf(yaw) * 100.0f, s.state.position[1],
                             s.state.position[2] - cosf(yaw) * 100.0f };
            uint32_t subtype = player->actor.colChkInfo.damage >= 8 ? Sm64::INT_SUBTYPE_BIG_KNOCKBACK : 0;
            lib().mario_take_damage(s.marioId, 0, subtype, src[0], src[1], src[2]);
            Player_MarioForceIdle(play, player);
            category = PLAYER_MARIO_ACTION_GROUND;
        } else if (category == PLAYER_MARIO_ACTION_LEDGE) {
            if (consented) {
                category = PLAYER_MARIO_ACTION_LINK;
            } else {
                Player_MarioForceIdle(play, player);
                category = PLAYER_MARIO_ACTION_GROUND;
            }
        }

        if (category == PLAYER_MARIO_ACTION_LINK || LinkIsBusy(play, player)) {
            s.mode = Mode::Link;
        }
    } else {
        bool linkFree = (category == PLAYER_MARIO_ACTION_GROUND || category == PLAYER_MARIO_ACTION_SETTLE) &&
                    !LinkIsBusy(play, player) && Message_GetState(&play->msgCtx) == TEXT_STATE_NONE;
        if (linkFree) {
            EnterMarioMode(player);
        }
    }

    if (s.mode == Mode::Link) {
        TickPuppet(play, player);
        return; // Link's logic runs with the real input
    }

    // --- Mario mode ---
    const Input raw = *input;
    u16 doAction = play->interfaceCtx.unk_1F0;
    bool passA = CHECK_BTN_ALL(raw.press.button, BTN_A) && IsLinkDoAction(doAction);
    bool holdA = CHECK_BTN_ALL(raw.cur.button, BTN_A) && doAction == DO_ACTION_GRAB;
    bool passC = CHECK_BTN_ANY(raw.press.button, BTN_CLEFT | BTN_CDOWN | BTN_CRIGHT | BTN_CUP | BTN_DUP |
                                                     BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT);
    bool assist = WantsLinkClimbAssist(play, player, raw);

    UpdateWaterLevel(play, player);
    SM64MarioInputs in = InputsFromController(play, raw, !(passA || holdA));
    Tick(TicksThisFrame(), in);
    ConvertMarioHealth(play);
    ApplyMarioToLink(player);

    // What Link's own logic gets to see this frame
    u16 removed = BTN_B | BTN_Z | BTN_R;
    if (!(passA || holdA)) {
        removed |= BTN_A;
    }
    input->cur.button &= ~removed;
    input->press.button &= ~removed;
    input->rel.button &= ~removed;
    if (!assist) {
        input->cur.stick_x = input->cur.stick_y = 0;
        input->rel.stick_x = input->rel.stick_y = 0;
        input->press.stick_x = input->press.stick_y = 0;
    }

    s.handedToLink = passA || holdA || passC || assist;
    s.pendingPostUpdate = true;

    if (CVarGetInteger(CVAR_MARIO("Debug"), 0) && (play->gameplayFrames % 10) == 0) {
        s32 exitIdx = player->actor.floorPoly != nullptr
                          ? SurfaceType_GetSceneExitIndex(&play->colCtx, player->actor.floorPoly, player->actor.floorBgId)
                          : -1;
        SPDLOG_INFO("[Mario] pos {:.1f} {:.1f} {:.1f} action {:08x} floorExit {} wall {} doors {} dyna {}",
                    player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z, s.state.action,
                    exitIdx, player->actor.wallPoly != nullptr, s.doors.size(), s.dyna.size());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Attacks
// ---------------------------------------------------------------------------------------------------------------

ColliderCylinderInit sAttackInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_PLAYER,
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK2,
        { DMG_SLASH_KOKIRI, 0x00, 0x01 },
        { 0x00000000, 0x00, 0x00 },
        TOUCH_ON | TOUCH_NEAREST | TOUCH_SFX_NORMAL,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { 16, 30, 0, { 0, 0, 0 } },
};

// Mario's punches, kicks, dives and ground pounds hurt enemies and break things like Link's sword and hammer do
void UpdateAttack(PlayState* play, Player* player) {
    if (!s.attackInit || s.attackPlay != play) {
        Collider_InitCylinder(play, &s.attack);
        Collider_SetCylinder(play, &s.attack, &player->actor, &sAttackInit);
        s.attackInit = true;
        s.attackPlay = play;
    }

    uint32_t action = s.state.action;
    if (!(action & Sm64::ACT_FLAG_ATTACKING)) {
        return;
    }

    float forward = 14.0f, radius = 16.0f, height = 30.0f;
    u32 dmgFlags = DMG_SLASH_KOKIRI;
    u8 damage = 1;

    if (action == Sm64::ACT_GROUND_POUND || action == Sm64::ACT_GROUND_POUND_LAND) {
        forward = 0.0f;
        radius = 30.0f;
        height = 20.0f;
        dmgFlags = DMG_HAMMER_JUMP | DMG_JUMP_MASTER;
        damage = 4;
    } else if (action == Sm64::ACT_DIVE || action == Sm64::ACT_SLIDE_KICK || action == Sm64::ACT_SLIDE_KICK_SLIDE ||
               action == Sm64::ACT_JUMP_KICK) {
        dmgFlags = DMG_SLASH_MASTER;
        damage = 2;
    }

    s16 yaw = player->actor.shape.rot.y;
    s.attack.dim.radius = (s16)radius;
    s.attack.dim.height = (s16)height;
    s.attack.dim.yShift = 0;
    s.attack.dim.pos.x = (s16)(player->actor.world.pos.x + Math_SinS(yaw) * forward);
    s.attack.dim.pos.y = (s16)(player->actor.world.pos.y);
    s.attack.dim.pos.z = (s16)(player->actor.world.pos.z + Math_CosS(yaw) * forward);
    s.attack.info.toucher.dmgFlags = dmgFlags;
    s.attack.info.toucher.damage = damage;

    CollisionCheck_SetAT(play, &play->colChkCtx, &s.attack.base);
}

// Runs at the end of Link's update
void OnPlayerUpdate() {
    if (!s.pendingPostUpdate || !IsMarioActive()) {
        return;
    }
    s.pendingPostUpdate = false;

    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);

    // Link's logic may have nudged him while settling; Mario's position wins
    if (s.mode == Mode::Mario) {
        ApplyMarioToLink(player);
        UpdateAttack(play, player);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------------------------

struct LightSetup {
    float dir[3];
    float tint[3];
};

LightSetup MakeLight(PlayState* play) {
    LightSetup l;
    Camera* cam = GET_ACTIVE_CAM(play);
    // Light from behind the camera and above, like SM64's own Mario lighting
    float dx = cam->eye.x - cam->at.x, dy = cam->eye.y - cam->at.y, dz = cam->eye.z - cam->at.z;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.001f) {
        dx = 0, dy = 0, dz = 1, len = 1;
    }
    dx /= len, dy /= len, dz /= len;
    dy += 1.0f;
    len = sqrtf(dx * dx + dy * dy + dz * dz);
    l.dir[0] = dx / len;
    l.dir[1] = dy / len;
    l.dir[2] = dz / len;
    // Darken Mario in dark scenes so he doesn't glow
    for (int i = 0; i < 3; i++) {
        float t = 0.55f + play->lightCtx.ambientColor[i] / 255.0f;
        l.tint[i] = t > 1.0f ? 1.0f : t;
    }
    return l;
}

// Pre-composited copies of Mario's textures over each base color they are drawn on (see DrawMario)
struct Composite {
    int tex;
    uint32_t color;
    alignas(16) uint8_t pixels[64 * 64 * 4];
};
constexpr int kMaxComposites = 48;
Composite sComposites[kMaxComposites];
int sNumComposites = 0;

int CompositeSlot(int tex, const float* triColor) {
    uint8_t r = (uint8_t)(triColor[0] * 255.0f), g = (uint8_t)(triColor[1] * 255.0f), b = (uint8_t)(triColor[2] * 255.0f);
    uint32_t key = (r << 16) | (g << 8) | b;
    for (int i = 0; i < sNumComposites; i++) {
        if (sComposites[i].tex == tex && sComposites[i].color == key) {
            return i;
        }
    }
    int slot = sNumComposites < kMaxComposites ? sNumComposites++ : kMaxComposites - 1;
    Composite& c = sComposites[slot];
    c.tex = tex;
    c.color = key;
    const uint8_t* src = sTextureChunks[tex];
    for (int p = 0; p < 64 * 64; p++) {
        float a = src[p * 4 + 3] / 255.0f;
        c.pixels[p * 4 + 0] = (uint8_t)(src[p * 4 + 0] * a + r * (1.0f - a));
        c.pixels[p * 4 + 1] = (uint8_t)(src[p * 4 + 1] * a + g * (1.0f - a));
        c.pixels[p * 4 + 2] = (uint8_t)(src[p * 4 + 2] * a + b * (1.0f - a));
        c.pixels[p * 4 + 3] = 255;
    }
    return slot;
}

} // namespace

// Outside the anonymous namespace: OPEN_DISPS declares OoT's frame interpolation functions at block scope,
// which would otherwise bind them to this file's anonymous namespace.
static void DrawMario(PlayState* play, Player* player) {
    int numTris = s.geo.numTrianglesUsed;
    if (numTris <= 0) {
        return;
    }

    // Hide Mario when the camera is inside him (first person aiming, looking around)
    Camera* cam = GET_ACTIVE_CAM(play);
    if (player->unk_6AD == 2 || Math_Vec3f_DistXYZ(&cam->eye, &player->actor.focus.pos) < 12.0f) {
        return;
    }

    GraphicsContext* gfxCtx = play->state.gfxCtx;
    LightSetup light = MakeLight(play);

    // Group triangles by texture and base color. Group 0 is untextured.
    // LUS's combiner can't light a texture blended over a per-vertex base color in one pass, so each
    // (texture, base color) pair is pre-composited into its own opaque texture and drawn as TEXEL0 * SHADE.
    std::vector<std::pair<int, std::vector<int>>> groups; // (composite slot or -1, triangles)
    std::unordered_map<int, size_t> groupIndex;
    for (int t = 0; t < numTris; t++) {
        float* uv = &sUvs[t * 6];
        int slot = -1;
        if (!(uv[0] == 1.0f && uv[1] == 1.0f && uv[2] == 1.0f && uv[3] == 1.0f && uv[4] == 1.0f && uv[5] == 1.0f)) {
            float u = (uv[0] + uv[2] + uv[4]) / 3.0f;
            int tex = (int)(u * kNumTextures);
            tex = tex < 0 ? 0 : (tex >= kNumTextures ? kNumTextures - 1 : tex);
            slot = CompositeSlot(tex, &sColors[t * 9]);
        }
        auto it = groupIndex.find(slot);
        if (it == groupIndex.end()) {
            groupIndex[slot] = groups.size();
            groups.push_back({ slot, { t } });
        } else {
            groups[it->second].second.push_back(t);
        }
    }

    // Build vertices in group order, relative to Mario's position, in SM64 units
    Vtx* vtx = (Vtx*)Graph_Alloc(gfxCtx, sizeof(Vtx) * numTris * 3);
    int written = 0;
    std::vector<int> groupStart(groups.size() + 1);
    for (size_t g = 0; g < groups.size(); g++) {
        groupStart[g] = written;
        int slot = groups[g].first;
        int tex = slot >= 0 ? sComposites[slot].tex : -1;
        for (int t : groups[g].second) {
            for (int k = 0; k < 3; k++) {
                int vi = t * 3 + k;
                Vtx* v = &vtx[written * 3 + k];
                v->v.ob[0] = (s16)lrintf(sPositions[vi * 3 + 0] - s.state.position[0]);
                v->v.ob[1] = (s16)lrintf(sPositions[vi * 3 + 1] - s.state.position[1]);
                v->v.ob[2] = (s16)lrintf(sPositions[vi * 3 + 2] - s.state.position[2]);
                v->v.flag = 0;

                if (tex >= 0) {
                    float local = sUvs[vi * 2 + 0] * kNumTextures - tex;
                    v->v.tc[0] = (s16)lrintf(local * 64.0f * 32.0f);
                    v->v.tc[1] = (s16)lrintf(sUvs[vi * 2 + 1] * 64.0f * 32.0f);
                } else {
                    v->v.tc[0] = v->v.tc[1] = 0;
                }

                float* n = &sNormals[vi * 3];
                float diffuse = n[0] * light.dir[0] + n[1] * light.dir[1] + n[2] * light.dir[2];
                float lit = 0.5f + 0.5f * (diffuse > 0.0f ? diffuse : 0.0f);
                float* c = &sColors[vi * 3];
                for (int ch = 0; ch < 3; ch++) {
                    // Untextured: base color * light. Textured: just the light (the color is in the texture).
                    float value = (tex >= 0 ? 1.0f : c[ch]) * light.tint[ch] * lit * 255.0f;
                    v->v.cn[ch] = (u8)(value > 255.0f ? 255 : (value < 0.0f ? 0 : value));
                }
                v->v.cn[3] = 255;
            }
            written++;
        }
    }
    groupStart[groups.size()] = written;

    OPEN_DISPS(gfxCtx);

    Matrix_Push();
    Matrix_Translate(player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z, MTXMODE_NEW);
    float inv = 1.0f / s.scale;
    Matrix_Scale(inv, inv, inv, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    Matrix_Pop();

    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gSPClearGeometryMode(POLY_OPA_DISP++,
                         G_LIGHTING | G_FOG | G_CULL_BOTH | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    gDPSetTextureLUT(POLY_OPA_DISP++, G_TT_NONE);
    gDPSetTexturePersp(POLY_OPA_DISP++, G_TP_PERSP);
    gDPSetTextureFilter(POLY_OPA_DISP++, G_TF_BILERP);

    for (size_t g = 0; g < groups.size(); g++) {
        int first = groupStart[g];
        int count = groupStart[g + 1] - first;
        int slot = groups[g].first;

        gDPPipeSync(POLY_OPA_DISP++);
        if (slot < 0) {
            gSPTexture(POLY_OPA_DISP++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
            gDPSetCombineLERP(POLY_OPA_DISP++, 0, 0, 0, SHADE, 0, 0, 0, 1, 0, 0, 0, SHADE, 0, 0, 0, 1);
        } else {
            gSPTexture(POLY_OPA_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
            gDPLoadTextureBlock(POLY_OPA_DISP++, sComposites[slot].pixels, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64, 0,
                                G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gDPSetCombineLERP(POLY_OPA_DISP++, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1);
        }

        for (int t = 0; t < count; t += 10) {
            int batch = count - t > 10 ? 10 : count - t;
            gSPVertex(POLY_OPA_DISP++, (uintptr_t)&vtx[(first + t) * 3], batch * 3, 0);
            for (int k = 0; k < batch; k += 2) {
                if (k + 1 < batch) {
                    gSP2Triangles(POLY_OPA_DISP++, k * 3, k * 3 + 1, k * 3 + 2, 0, (k + 1) * 3, (k + 1) * 3 + 1,
                                  (k + 1) * 3 + 2, 0);
                } else {
                    gSP1Triangle(POLY_OPA_DISP++, k * 3, k * 3 + 1, k * 3 + 2, 0);
                }
            }
        }
    }

    // Put the state back the way OoT's draw code expects it
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_LIGHTING | G_FOG | G_CULL_BACK);
    gSPTexture(POLY_OPA_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);

    CLOSE_DISPS(gfxCtx);
}

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------------------------

void OnSceneOrPlayEnd() {
    Teardown();
}

void RegisterMarioReplacesLink() {
    bool enabled = CVAR_MARIO_ENABLED_VALUE;

    if (!enabled) {
        Teardown();
        Sm64::StopAudio();
    } else {
        Sm64::SetVolume(CVarGetFloat(CVAR_MARIO("Volume"), 1.0f));
    }

    COND_VB_SHOULD(VB_PLAYER_UPDATE_COMMON, enabled, {
        Player* player = va_arg(args, Player*);
        Input* input = va_arg(args, Input*);
        if (player->actor.id != ACTOR_PLAYER) {
            return; // Dark Link stays Link
        }
        OnPlayerUpdateCommon(player, input);
    });

    COND_HOOK(OnPlayerUpdate, enabled, OnPlayerUpdate);

    COND_VB_SHOULD(VB_DRAW_PLAYER_MODEL, enabled, {
        Player* player = va_arg(args, Player*);
        int32_t isReflection = va_arg(args, int32_t);
        if (player->actor.id != ACTOR_PLAYER || !IsMarioActive()) {
            return;
        }
        *should = false;
        if (!isReflection) {
            DrawMario(gPlayState, player);
        }
    });

    COND_HOOK(OnPlayDestroy, enabled, OnSceneOrPlayEnd);
}

static RegisterShipInitFunc initFunc(RegisterMarioReplacesLink,
                                     { CVAR_MARIO_ENABLED_NAME, CVAR_MARIO("Size"), CVAR_MARIO("Volume") });

} // namespace

// Used by the menu to show whether the library and the ROM were found
extern "C" const char* Mario_GetStatus() {
    if (CVAR_MARIO_ENABLED_VALUE) {
        Sm64::EnsureLoaded();
    }
    return Sm64::Status().c_str();
}

extern "C" void Mario_RetryLoad() {
    Teardown();
    Sm64::Reset();
    Sm64::EnsureLoaded();
}
