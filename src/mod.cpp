#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"

// Game includes
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "dusk/map_loader_definitions.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_Reset.h"

#include <random>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

bool betaQuestRunning = true;

// Example game hook: turn heart drops into green rupees.

// Use the mangled name because it's overloaded :(
DEFINE_HOOK_SYMBOL("_Z21dComIfGp_setNextStagePKcsaafjiasii",
                   void(char const *, s16, s8, s8, f32, u32, int, s8, s16, int, int), setNextStage);

DEFINE_HOOK_SYMBOL("dSv_info_c::init", void(dSv_info_init *), dSv_info_init);
DEFINE_HOOK_SYMBOL("daAlink_c::procGrassWhistleWait", void(daAlink_c *), procGrassWhistleWait);

DEFINE_HOOK(mDoRst_reset, doRst_reset);

constexpr u32 getRandomIndex(std::mt19937 &gen, int size) {
    std::uniform_int_distribution<int> dist(0, size - 1);
    return dist(gen);
}

static HookAction dComIfGp_setNextStage_pre(ModContext *ctx, void *args, void *, void *) {
    const char *&i_stage = mods::arg_ref<const char *>(args, 0);
    s16 &i_point = mods::arg_ref<s16>(args, 1);
    s8 &i_roomNo = mods::arg_ref<s8>(args, 2);
    s8 &i_layer = mods::arg_ref<s8>(args, 3);

    base_process_class *playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (betaQuestRunning == false || playScene == nullptr) {
        return HOOK_CONTINUE;
    }

    std::random_device rd;

    std::hash<std::string> string_hash;
    std::mt19937 gen(string_hash(std::string(dComIfGs_getPlayerName()) + i_stage + std::to_string(i_point) +
                                 std::to_string(i_roomNo) + std::to_string(i_layer)));

    // Region

    const RegionEntry &region = gameRegions[getRandomIndex(gen, gameRegions.size())];
    const MapEntry &mapEntry = region.maps[getRandomIndex(gen, region.maps.size())];
    const RoomEntry &roomEntry = mapEntry.mapRooms[getRandomIndex(gen, mapEntry.mapRooms.size())];
    const s16 point = roomEntry.roomPoints[getRandomIndex(gen, roomEntry.roomPoints.size())];

    i_stage = mapEntry.mapFile;
    i_roomNo = roomEntry.roomNo;
    i_point = point;
    i_layer = -1;

    svc_log->info(ctx, i_stage);
    return HOOK_CONTINUE;
}

static void dSv_info_init_post(ModContext *ctx, void *args, void *, void *) {
    dComIfGs_setSelectEquipSword(dItemNo_WOOD_STICK_e);
    dComIfGs_setSelectEquipClothes(dItemNo_WEAR_KOKIRI_e);
    dComIfGs_setItem(SLOT_21, dItemNo_HORSE_FLUTE_e);
    dComIfGs_onDarkClearLV(3);

    // betaQuestRunning = true;
}

static HookAction grassWhistlePre(ModContext *ctx, void *args, void *, void *) {
    daHorse_c *horse_p = (daHorse_c *)dComIfGp_getHorseActor();
    if (horse_p == nullptr) {
        cXyz pos = {0.0f, fopAcM_GetPosition(dComIfGp_getPlayer(LINK_PTR)).y, 0.0f};
        cXyz scale = {1.0f, 1.0f, 1.0f};
        csXyz angle = {};
        unsigned int result =
            fopAcM_create(fpcNm_HORSE_e, 0, &pos, fopAcM_GetRoomNo(dComIfGp_getPlayer(LINK_PTR)), &angle, &scale, 0);
    }
    return HOOK_CONTINUE;
}

static HookAction doReset_pre(ModContext *ctx, void *args, void *, void *) {
    betaQuestRunning = false;
    return HOOK_CONTINUE;
}

#define HOOK_ADD_PRE(alias, callback)                                                                                  \
    {                                                                                                                  \
        ModResult result = mods::hook_add_pre<alias>(svc_hook, callback);                                              \
        if (result != MOD_OK) {                                                                                        \
            svc_log->error(mod_ctx, "failed to install " #callback);                                                   \
            return result;                                                                                             \
        }                                                                                                              \
    }

#define HOOK_ADD_POST(alias, callback)                                                                                 \
    {                                                                                                                  \
        ModResult result = mods::hook_add_post<alias>(svc_hook, callback);                                             \
        if (result != MOD_OK) {                                                                                        \
            svc_log->error(mod_ctx, "failed to install " #callback);                                                   \
            return result;                                                                                             \
        }                                                                                                              \
    }

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError *) {
    // Installs a pre hook on fopAcM_createItem.
    HOOK_ADD_PRE(setNextStage, dComIfGp_setNextStage_pre);
    HOOK_ADD_PRE(doRst_reset, doReset_pre);
    HOOK_ADD_POST(dSv_info_init, dSv_info_init_post);
    HOOK_ADD_PRE(procGrassWhistleWait, grassWhistlePre);

    svc_log->info(mod_ctx, "beta quest initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError *) { return MOD_OK; }

MOD_EXPORT ModResult mod_shutdown(ModError *) { return MOD_OK; }
}
