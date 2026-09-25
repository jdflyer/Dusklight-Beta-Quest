#include "mods/service.hpp"
#include "mods/svc/game_mode.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/save.h"
#include "mods/svc/ui.h"

// Game includes
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "f_op/f_op_actor_mng.h"

#include "entrance_definitions.hpp"

#include <algorithm>
#include <assert.h>
#include <random>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(GameModeService, svc_gamemode);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(SaveService, svc_save);

#define BETA_QUEST_GAMEMODE_ID "beta-quest"
#define SETTINGS_SAVE_BLOB "jdflyer_Betaquest_Settings"
#define ENTRANCE_ARRAY_SAVE_BLOB "jdflyer_Betaquest_EntranceArray"

// Use the mangled name because it's overloaded :(
#ifdef _MSVC_LANG
#define setNextStage_sig "?dComIfGp_setNextStage@@YAXPEBDFCCMIHCFHH@Z"
#else
#define setNextStage_sig "_Z21dComIfGp_setNextStagePKcsaafjiasii"
#endif

DEFINE_HOOK_SYMBOL(setNextStage_sig, void(char const *, s16, s8, s8, f32, u32, int, s8, s16, int, int), setNextStage);
DEFINE_HOOK(&daAlink_c::procGrassWhistleWait, procGrassWhistleWait);
DEFINE_HOOK_SYMBOL("dStage_playerInit", int(dStage_dt_c *, void *, int, void *), stage_playerInit);

enum BetaQuestSword_e { SWORD_NONE, SWORD_WOODEN, SWORD_ORDON, SWORD_MASTER, SWORD_MAX };

struct BetaQuestSaveSettings {
    uint32_t version = 1;
    bool twilightsCleared = true;
    bool startWithEponaCall = true;
    bool eponaAnywhere = true;
    bool shadowCrystal = true;
    BetaQuestSword_e startingSword = SWORD_ORDON;
    char seed[32] = "insert_seed_here";
};
BetaQuestSaveSettings saveSettings;

struct EntranceEntry {
    char mStageName[8];
    s16 mRoomNo;
    s16 mPoint;

    constexpr EntranceEntry() : mStageName(""), mRoomNo(0), mPoint(0) {}
    constexpr EntranceEntry(std::string_view stageName, s16 roomNo, s16 point)
        : mStageName{}, mRoomNo(roomNo), mPoint(point) {
        stageName.copy(mStageName, sizeof(mStageName) - 1);
    };
    constexpr bool operator==(const EntranceEntry &other) const {
        return mRoomNo == other.mRoomNo && mPoint == other.mPoint && std::strcmp(mStageName, other.mStageName) == 0;
    }
};

struct EntranceEntryHash {
    std::size_t operator()(const EntranceEntry &e) const {
        std::size_t h1 = std::hash<std::string_view>{}(e.mStageName);
        std::size_t h2 = std::hash<s16>{}(e.mRoomNo);
        std::size_t h3 = std::hash<s16>{}(e.mPoint);
        // simple combine
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

struct BetaQuestBinaryEntranceData {
    uint32_t version = 1;
    size_t entryNum;
    EntranceEntry entries[];
};

constexpr u32 getRandomIndex(std::mt19937 &gen, int size) {
    std::uniform_int_distribution<int> dist(0, size - 1);
    return dist(gen);
}

std::unordered_map<EntranceEntry, EntranceEntry, EntranceEntryHash> entranceMap;
std::vector<EntranceEntry> mainEntryArray;

// From hooks.cpp in the randomizer mod
HookAction hookPreStagePlayerInit(ModContext *, void *args, void *retval, void *) {
    void *i_data = mods::arg<void *>(args, 1);
    int num = mods::arg<int>(args, 2);

    stage_actor_class *player = (stage_actor_class *)((int *)i_data + 1);
    stage_actor_data_class *player_data = player->m_entries;

    // Modify entrance types in certain situations to avoid crashes
    for (size_t i = 0; i < num; ++i) {
        u8 &entranceType = reinterpret_cast<u8 *>(&player_data[i].base.parameters)[2];
        switch (entranceType) {
        // Only replace the entrance type if it is a door.
        case 0x80:
        case 0xA0:
        case 0xB0: {
            if (dComIfGs_getTransformStatus() == TF_STATUS_WOLF) {
                // Change the entrance type to play the animation of walking out of the
                // loading zone instead of entering through the door.
                entranceType = 0x50;
            }
            break;
        }

        // Water swimming entrance.
        // If we have this, but there isn't any water to spawn in, the game hangs
        case 0xD0:
            entranceType = 0x50;
            break;
        default:
            break;
        }
    }

    return HOOK_CONTINUE;
}

static HookAction dComIfGp_setNextStage_pre(ModContext *ctx, void *args, void *, void *) {
    const char *&i_stage = mods::arg_ref<const char *>(args, 0);
    s16 &i_point = mods::arg_ref<s16>(args, 1);
    s8 &i_roomNo = mods::arg_ref<s8>(args, 2);
    s8 &i_layer = mods::arg_ref<s8>(args, 3);
    u32 &lastMode = mods::arg_ref<u32>(args, 5);
    mods::log::debug("EntranceEntry(\"{}\",{},{},{}),", i_stage, i_roomNo, i_point, lastMode);

    base_process_class *playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene == nullptr) {
        return HOOK_CONTINUE;
    }

    if (i_point == -1) {
        i_point = dComIfGs_getStartPoint();
    }

    const auto &it = entranceMap.find(EntranceEntry(i_stage, i_roomNo, i_point));
    if (it == entranceMap.end()) {
        return HOOK_CONTINUE;
    }

    EntranceEntry &entry = it->second;

    i_stage = entry.mStageName;
    i_roomNo = entry.mRoomNo;
    i_point = entry.mPoint;

    // Override wolf dig hole entrances
    if ((lastMode & 0xF) == 9) {
        lastMode = (lastMode & 0xFFFFFFF0);
    }

    // Don't use epona through entrances
    if ((lastMode & 0xF) == 1 || (lastMode & 0xF) == 8) {
        lastMode = (lastMode & 0xFFFFFFF0);
    }

    mods::log::debug("Overridden EntranceEntry(\"{}\",{},{},{}),", i_stage, i_roomNo, i_point, lastMode);

    return HOOK_CONTINUE;
}

constexpr uint64_t hash_64(std::string_view str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static ModResult onNewSave(void *, ModError *) {
    switch (saveSettings.startingSword) {
    case SWORD_NONE:
    default:
        break;
    case SWORD_WOODEN:
        dComIfGs_setSelectEquipSword(dItemNo_WOOD_STICK_e);
        break;
    case SWORD_ORDON:
        dComIfGs_setSelectEquipSword(dItemNo_SWORD_e);
        break;
    case SWORD_MASTER:
        dComIfGs_setSelectEquipSword(dItemNo_MASTER_SWORD_e);
        break;
    }

    if (saveSettings.startWithEponaCall) {
        dComIfGs_setItem(SLOT_21, dItemNo_HORSE_FLUTE_e);
    }

    if (saveSettings.twilightsCleared) {
        dComIfGs_setSelectEquipClothes(dItemNo_WEAR_KOKIRI_e);
        // Clear the twilights
        dComIfGs_onDarkClearLV(0);
        dComIfGs_onDarkClearLV(1);
        dComIfGs_onDarkClearLV(2);
        dComIfGs_onDarkClearLV(3);
    }

    if (saveSettings.shadowCrystal) {
        // Allow midna everywhere
        dComIfGs_onTransformLV(0);
        dComIfGs_onTransformLV(1);
        dComIfGs_onTransformLV(2);
        dComIfGs_onTransformLV(3);

        /* Main Event - Get shadow crystal (can now transform) */
        dComIfGs_onEventBit(dSv_event_flag_c::M_077);
        /* Main Event - when OFF, wolf carries sword and shield on back */
        dComIfGs_onEventBit(dSv_event_flag_c::M_068);
        /* dSv_event_flag_c::F_0550 - Main Event - Gain ability to use sense */
        dComIfGs_onEventBit(dSv_event_flag_c::saveBitLabels[550]);
        /* Midna riding / not riding (ON == riding) */
        dComIfGs_onEventBit(dSv_event_flag_c::M_067);
        dComIfGs_onEventBit(dSv_event_flag_c::M_021);  // Portal Warping enabled
        dComIfGs_onEventBit(dSv_event_flag_c::M_015);  // Midna B Attack
        dComIfGs_onEventBit(dSv_event_flag_c::F_0250); // No White Midna
    }

    svc_save->set_blob(mod_ctx, SETTINGS_SAVE_BLOB, &saveSettings, sizeof(saveSettings));

    std::vector<EntranceEntry> entries;
    for (const auto &region : gameRegions) {
        for (const auto &map : region.maps) {
            for (const auto &room : map.mapRooms) {
                for (const auto &point : room.roomPoints) {
                    entries.push_back(EntranceEntry(map.mapFile, room.roomNo, point));
                }
            }
        }
    }

    // Shuffle entries according to the seed
    std::mt19937 gen(hash_64(std::string(saveSettings.seed)));
    std::shuffle(entries.begin(), entries.end(), gen);

    size_t entranceBinarySize = sizeof(EntranceEntry) * entries.size();
    std::vector<u8> binaryData = std::vector<u8>(sizeof(BetaQuestBinaryEntranceData) + entranceBinarySize);

    BetaQuestBinaryEntranceData &fullArray = *(BetaQuestBinaryEntranceData *)binaryData.data();
    fullArray.version = 1;
    fullArray.entryNum = entries.size();

    std::memcpy(binaryData.data() + sizeof(BetaQuestBinaryEntranceData), entries.data(), entranceBinarySize);

    svc_save->set_blob(mod_ctx, ENTRANCE_ARRAY_SAVE_BLOB, binaryData.data(), binaryData.size());
    return MOD_OK;
}

static ModResult onSaveLoad(void *, ModError *) {
    entranceMap = {};

    size_t saveSize = sizeof(BetaQuestSaveSettings);
    svc_save->get_blob(mod_ctx, SETTINGS_SAVE_BLOB, &saveSettings, &saveSize);
    assert(saveSize == sizeof(BetaQuestSaveSettings));
    assert(saveSettings.version == 1);

    size_t entranceDataSize = 0;
    svc_save->get_blob(mod_ctx, ENTRANCE_ARRAY_SAVE_BLOB, nullptr, &entranceDataSize);
    assert(entranceDataSize >= sizeof(BetaQuestBinaryEntranceData));
    std::vector<uint8_t> binaryData(entranceDataSize);
    const BetaQuestBinaryEntranceData &entranceData = *(BetaQuestBinaryEntranceData *)binaryData.data();
    svc_save->get_blob(mod_ctx, ENTRANCE_ARRAY_SAVE_BLOB, binaryData.data(), &entranceDataSize);
    assert(entranceData.version == 1);
    assert(entranceDataSize ==
           sizeof(BetaQuestBinaryEntranceData) + sizeof(entranceData.entries[0]) * entranceData.entryNum);

    // Our mapping from entrance to entrance is to always map the current entrance to the one earlier in the list
    for (int i = 1; i < entranceData.entryNum; i++) {
        const EntranceEntry &entry = entranceData.entries[i];
        entranceMap[entry] = entranceData.entries[i - 1];
    }
    entranceMap[entranceData.entries[0]] = entranceData.entries[entranceData.entryNum - 1];

    // dComIfGs_getSaveData()->mPlayer.getPlayerReturnPlace().set("F_SP103", 1, 1);
    // dComIfGp_setNextStage("F_SP103", 1, 1, -1);
    // g_dComIfG_gameInfo.play.mNextStage.getStartStage()->set("F_SP103", 1, 1, -1);
    return MOD_OK;
}

static HookAction grassWhistlePre(ModContext *ctx, void *args, void *, void *) {
    if (!saveSettings.eponaAnywhere) {
        return HOOK_CONTINUE;
    }
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

void addUIBool(UiElementHandle pane, bool *val, const char *name, const char *description, UiWindowHandle *out_handle) {
    UiControlDesc desc = UI_CONTROL_DESC_INIT;
    desc.kind = UI_CONTROL_TOGGLE;
    desc.label = name;
    desc.help_rml = description;
    desc.binding = UI_BINDING_CALLBACKS;
    desc.user_data = val;
    desc.set = [](ModContext *ctx, void *userdata, const struct UiControlValue *new_val) {
        // *(bool*)userdata = *(bool*)userdata;
        *(bool *)userdata = new_val->bool_value;
    };
    desc.get = [](ModContext *ctx, void *userdata, UiControlValue *out_value) {
        out_value->bool_value = *(bool *)userdata;
    };
    svc_ui->pane_add_control(mod_ctx, pane, &desc, out_handle);
}

ModResult onNewSaveSelect(void *, GameModeNewSaveState *state, ModError *) {
    static GameModeNewSaveState *newSaveState;
    static UiWindowHandle windowHandle;

    newSaveState = state;

    UiTabDesc tabs[1]{};

    tabs[0].struct_size = sizeof(UiTabDesc);
    tabs[0].title = "Play";
    tabs[0].build = [](ModContext *ctx, UiWindowHandle, UiElementHandle leftPane, UiElementHandle rightPane,
                       void *userdata, ModError *) {
        addUIBool(leftPane, &saveSettings.twilightsCleared, "Twilights Cleared", "Start With All Twilights Cleared",
                  nullptr);
        addUIBool(leftPane, &saveSettings.startWithEponaCall, "Epona Call", "Start With Epona Call", nullptr);
        addUIBool(leftPane, &saveSettings.eponaAnywhere, "Epona Anywhere", "Allows Epona To Be Used Anywhere", nullptr);
        addUIBool(leftPane, &saveSettings.shadowCrystal, "Shadow Crystal", "Start With The Ability To Transform",
                  nullptr);

        UiControlDesc desc = UI_CONTROL_DESC_INIT;
        desc.kind = UI_CONTROL_SELECT;
        desc.label = "Starting Sword";
        desc.help_rml = "The Sword Link Should Start With";
        desc.option_count = SWORD_MAX;
        const char *options[4] = {"No Sword", "Wooden Sword", "Ordon Sword", "Master Sword"};
        desc.options = options;
        desc.user_data = &saveSettings.startingSword;
        desc.set = [](ModContext *ctx, void *userdata, const struct UiControlValue *new_val) {
            *(BetaQuestSword_e *)userdata = (BetaQuestSword_e)new_val->int_value;
        };
        desc.get = [](ModContext *ctx, void *userdata, UiControlValue *out_value) {
            out_value->int_value = *(BetaQuestSword_e *)userdata;
        };
        svc_ui->pane_add_control(mod_ctx, leftPane, &desc, nullptr);

        desc = UI_CONTROL_DESC_INIT;
        desc.kind = UI_CONTROL_STRING;
        desc.label = "Seed";
        desc.help_rml = "The Seed To Use for random generation";
        desc.user_data = &saveSettings.seed;
        desc.set = [](ModContext *ctx, void *userdata, const struct UiControlValue *new_val) {
            std::strncpy((char *)userdata, new_val->string_value, 31);
        };
        desc.get = [](ModContext *ctx, void *userdata, UiControlValue *out_value) {
            out_value->string_value = (char *)userdata;
        };
        svc_ui->pane_add_control(mod_ctx, leftPane, &desc, nullptr);

        desc = UI_CONTROL_DESC_INIT;
        desc.kind = UI_CONTROL_BUTTON;
        desc.label = "Play";
        desc.help_rml = "Play Button";
        desc.on_pressed = [](ModContext *ctx, void *userdata) {
            *newSaveState = GAME_MODE_STATE_PROCEED;
            svc_ui->window_close(ctx, windowHandle);
        };
        svc_ui->pane_add_control(mod_ctx, leftPane, &desc, nullptr);
        return MOD_OK;
    };

    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = [](ModContext *, UiWindowHandle, void *userdata) {
        bool *out_returnToFileSelect = static_cast<bool *>(userdata);

        // if closing the window through backing out, return to file select
        if (*newSaveState == GAME_MODE_STATE_PENDING) {
            *newSaveState = GAME_MODE_STATE_RETURN;
        }
    };

    svc_ui->window_push(mod_ctx, &desc, &windowHandle);

    return MOD_OK;
}

#define HOOK_ADD_PRE(alias, callback)                                                                                  \
    {                                                                                                                  \
        ModResult result = mods::hook::add_pre<alias>(svc_hook, callback);                                             \
        if (result != MOD_OK) {                                                                                        \
            svc_log->error(mod_ctx, "failed to install " #callback);                                                   \
        }                                                                                                              \
    }

#define HOOK_ADD_POST(alias, callback)                                                                                 \
    {                                                                                                                  \
        ModResult result = mods::hook::add_post<alias>(svc_hook, callback);                                            \
        if (result != MOD_OK) {                                                                                        \
            svc_log->error(mod_ctx, "failed to install " #callback);                                                   \
        }                                                                                                              \
    }
#define HOOK_UNINSTALL(alias)                                                                                          \
    {                                                                                                                  \
        ModResult result = mods::hook::uninstall<alias>();                                                             \
        if (result != MOD_OK) {                                                                                        \
            svc_log->error(mod_ctx, "failed to uninstall " #alias);                                                    \
        }                                                                                                              \
    }

UiMenuTabHandle g_modtab;
ModResult onGamemodeActivated(void *, ModError *outError) {
    HOOK_ADD_PRE(setNextStage, dComIfGp_setNextStage_pre);
    HOOK_ADD_PRE(procGrassWhistleWait, grassWhistlePre);
    HOOK_ADD_PRE(stage_playerInit, hookPreStagePlayerInit);


    UiMenuTabDesc desc = UI_MENU_TAB_DESC_INIT;
    desc.label = "Beta Quest";
    desc.on_selected = [](ModContext *ctx, void *user_data) {
        static UiWindowHandle windowHandle;
        UiTabDesc tabs[1]{};

        tabs[0].struct_size = sizeof(UiTabDesc);
        tabs[0].title = "Game Options";
        tabs[0].build = [](ModContext *ctx, UiWindowHandle, UiElementHandle leftPane, UiElementHandle rightPane,
                           void *userdata, ModError *) {
            UiControlDesc desc;
            desc = UI_CONTROL_DESC_INIT;
            desc.kind = UI_CONTROL_BUTTON;
            desc.label = "Return to spawn";
            desc.help_rml = "Return to the first randomized entrance";
            desc.on_pressed = [](ModContext *ctx, void *userdata) {
                base_process_class *playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
                if (playScene != nullptr) {
                    dComIfGp_setNextStage("F_SP103", 1, 1, -1);
                }
                svc_ui->window_close(ctx, windowHandle);
            };
            svc_ui->pane_add_control(mod_ctx, leftPane, &desc, nullptr);
            return MOD_OK;
        };

        UiWindowDesc desc = UI_WINDOW_DESC_INIT;
        desc.tabs = tabs;
        desc.tab_count = 1;

        svc_ui->window_push(mod_ctx, &desc, &windowHandle);
    };
    svc_ui->register_menu_tab(mod_ctx, &desc, &g_modtab);

    mods::log::info("Beta Quest Initialized");
    return MOD_OK;
}

ModResult onGamemodeDeactivated(void *, ModError *) {
    HOOK_UNINSTALL(setNextStage)
    HOOK_UNINSTALL(procGrassWhistleWait)

    svc_ui->unregister_menu_tab(mod_ctx, g_modtab);

    return MOD_OK;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError *) {
    const GameModeDesc gamemodeDesc = {
        .struct_size = sizeof(GameModeDesc),
        .game_mode_id = BETA_QUEST_GAMEMODE_ID,
        .full_name = "Beta Quest",
        .save_name = "beta-quest",
        .on_activated = onGamemodeActivated,
        .on_deactivated = onGamemodeDeactivated,
        .on_save_loaded = onSaveLoad,
        .on_new_save = onNewSave,
        .on_new_save_select = onNewSaveSelect,
    };

    return svc_gamemode->register_game_mode(mod_ctx, &gamemodeDesc);
}

MOD_EXPORT ModResult mod_update(ModError *) { return MOD_OK; }

MOD_EXPORT ModResult mod_shutdown(ModError *) { return MOD_OK; }
}
