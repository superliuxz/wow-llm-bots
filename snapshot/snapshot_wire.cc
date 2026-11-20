// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "glaze/glaze.hpp"  // IWYU pragma: keep

// Enum names: the wire format of every enum in snapshot.h.

template <>
struct glz::meta<wlb::snapshot::Command_Act::Verb> {
  using enum wlb::snapshot::Command_Act::Verb;
  static constexpr auto value = glz::enumerate(VERB_UNSPECIFIED,
                                               TO,
                                               AWAY,
                                               TARGET,
                                               USE,
                                               STOP,
                                               WAKE,
                                               AVOID,
                                               FACE,
                                               FACE_AWAY,
                                               JUMP,
                                               CANCEL,
                                               MARK,
                                               INTERACT,
                                               BOARD,
                                               LEAVE,
                                               EQUIP,
                                               PET_ATTACK,
                                               PET,
                                               KITE);
};

template <>
struct glz::meta<wlb::snapshot::Command_Act::PlaceKind> {
  using enum wlb::snapshot::Command_Act::PlaceKind;
  static constexpr auto value = glz::enumerate(PLACE_UNSPECIFIED,
                                               AT,
                                               BEHIND,
                                               IN_FRONT,
                                               MAX_RANGE,
                                               YD_FROM);
};

template <>
struct glz::meta<wlb::snapshot::Command_Act::ThingKind> {
  using enum wlb::snapshot::Command_Act::ThingKind;
  static constexpr auto value = glz::enumerate(THING_UNSPECIFIED,
                                               ME,
                                               TANK,
                                               OFFTANK,
                                               HEALER,
                                               LEADER,
                                               ANYONE,
                                               BOSS,
                                               IT,
                                               MY_TARGET,
                                               FOCUS,
                                               MARKER,
                                               NEAREST_ALLY,
                                               NEAREST_PLAYER,
                                               NEAREST_ENEMY,
                                               NEAREST_NAMED,
                                               ALLY_WITH,
                                               GROUP,
                                               ALLIES,
                                               NAMED);
};

template <>
struct glz::meta<wlb::snapshot::Command_Act::StopKind> {
  using enum wlb::snapshot::Command_Act::StopKind;
  static constexpr auto value =
      glz::enumerate(STOP_UNSPECIFIED, CASTING, ATTACKING, MOVING);
};

template <>
struct glz::meta<wlb::snapshot::Command_Act::PetCmd> {
  using enum wlb::snapshot::Command_Act::PetCmd;
  static constexpr auto value =
      glz::enumerate(PET_CMD_UNSPECIFIED, STAY, FOLLOW, PASSIVE, DEFENSIVE);
};

template <>
struct glz::meta<wlb::snapshot::Direction> {
  using enum wlb::snapshot::Direction;
  static constexpr auto value =
      glz::enumerate(DIRECTION_UNSPECIFIED, HERE, N, NE, E, SE, S, SW, W, NW);
};

template <>
struct glz::meta<wlb::snapshot::DispelType> {
  using enum wlb::snapshot::DispelType;
  static constexpr auto value = glz::enumerate(DISPEL_TYPE_UNSPECIFIED,
                                               DISPEL_NONE,
                                               MAGIC,
                                               CURSE,
                                               POISON,
                                               DISEASE);
};

template <>
struct glz::meta<wlb::snapshot::RaidMarker> {
  using enum wlb::snapshot::RaidMarker;
  static constexpr auto value = glz::enumerate(RAID_MARKER_UNSPECIFIED,
                                               STAR,
                                               CIRCLE,
                                               DIAMOND,
                                               TRIANGLE,
                                               MOON,
                                               SQUARE,
                                               CROSS,
                                               SKULL);
};

template <>
struct glz::meta<wlb::snapshot::Role> {
  using enum wlb::snapshot::Role;
  static constexpr auto value =
      glz::enumerate(ROLE_UNSPECIFIED, TANK, HEALER, DPS);
};

template <>
struct glz::meta<wlb::snapshot::Stat> {
  using enum wlb::snapshot::Stat;
  static constexpr auto value = glz::enumerate(STAT_UNSPECIFIED,
                                               STRENGTH,
                                               AGILITY,
                                               STAMINA,
                                               INTELLECT,
                                               SPIRIT,
                                               ARMOR,
                                               HEALTH,
                                               MANA,
                                               ATTACK_POWER,
                                               RANGED_ATTACK_POWER,
                                               SPELL_POWER,
                                               MP5,
                                               HIT_RATING,
                                               CRIT_RATING,
                                               HASTE_RATING,
                                               EXPERTISE_RATING,
                                               ARMOR_PENETRATION_RATING,
                                               SPELL_PENETRATION,
                                               DEFENSE_RATING,
                                               DODGE_RATING,
                                               PARRY_RATING,
                                               BLOCK_RATING,
                                               BLOCK_VALUE,
                                               RESILIENCE_RATING,
                                               FIRE_RESISTANCE,
                                               FROST_RESISTANCE,
                                               NATURE_RESISTANCE,
                                               SHADOW_RESISTANCE,
                                               ARCANE_RESISTANCE,
                                               WEAPON_DPS,
                                               WEAPON_MIN_DAMAGE,
                                               WEAPON_MAX_DAMAGE,
                                               WEAPON_SPEED,
                                               MELEE_CRIT_PCT,
                                               RANGED_CRIT_PCT,
                                               SPELL_CRIT_PCT,
                                               MELEE_HIT_PCT,
                                               SPELL_HIT_PCT,
                                               HASTE_PCT,
                                               EXPERTISE,
                                               DEFENSE,
                                               DODGE_PCT,
                                               PARRY_PCT,
                                               BLOCK_PCT);
};

template <>
struct glz::meta<wlb::snapshot::Panel> {
  using enum wlb::snapshot::Panel;
  static constexpr auto value = glz::enumerate(PANEL_UNSPECIFIED,
                                               MAP,
                                               BAGS,
                                               CHARACTER,
                                               QUEST_LOG,
                                               SPELLBOOK,
                                               TALENTS,
                                               SKILLS,
                                               REPUTATION,
                                               SOCIAL,
                                               CALENDAR,
                                               ACHIEVEMENTS,
                                               CURRENCY,
                                               PVP,
                                               COMPANIONS,
                                               DUNGEON_FINDER,
                                               SCOREBOARD);
};

template <>
struct glz::meta<wlb::snapshot::ChatChannel> {
  using enum wlb::snapshot::ChatChannel;
  static constexpr auto value = glz::enumerate(CHAT_CHANNEL_UNSPECIFIED,
                                               SAY,
                                               YELL,
                                               WHISPER,
                                               WHISPER_SENT,
                                               PARTY,
                                               RAID,
                                               RAID_WARNING,
                                               GUILD,
                                               OFFICER,
                                               GENERAL,
                                               TRADE,
                                               LFG,
                                               EMOTE,
                                               MONSTER_SAY,
                                               MONSTER_YELL,
                                               MONSTER_EMOTE,
                                               MONSTER_WHISPER,
                                               BOSS_EMOTE,
                                               SYSTEM,
                                               CUSTOM_CHANNEL,
                                               BATTLEGROUND,
                                               ACHIEVEMENT);
};

template <>
struct glz::meta<wlb::snapshot::Self_Indicators_Durability::Level> {
  using enum wlb::snapshot::Self_Indicators_Durability::Level;
  static constexpr auto value =
      glz::enumerate(LEVEL_UNSPECIFIED, DAMAGED, BROKEN);
};

template <>
struct glz::meta<wlb::snapshot::GoalPart::Kind> {
  using enum wlb::snapshot::GoalPart::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               SELL,
                                               BUY,
                                               TRAIN,
                                               REPAIR,
                                               LEARN_PROFESSION,
                                               REACH_LEVEL,
                                               TRAVEL_TO,
                                               MEET,
                                               FARM,
                                               OTHER);
};

template <>
struct glz::meta<wlb::snapshot::HoverRef::Kind> {
  using enum wlb::snapshot::HoverRef::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               ITEM,
                                               SPELL,
                                               AURA,
                                               UNIT,
                                               ZONE,
                                               PLACE,
                                               QUEST,
                                               ACHIEVEMENT,
                                               OBJECT);
};

template <>
struct glz::meta<wlb::snapshot::Calendar_Entry::Status> {
  using enum wlb::snapshot::Calendar_Entry::Status;
  static constexpr auto value = glz::enumerate(STATUS_UNSPECIFIED,
                                               INVITED,
                                               ACCEPTED,
                                               DECLINED,
                                               TENTATIVE);
};

template <>
struct glz::meta<wlb::snapshot::DisplacedEvent::How> {
  using enum wlb::snapshot::DisplacedEvent::How;
  static constexpr auto value =
      glz::enumerate(HOW_UNSPECIFIED, KNOCKED_BACK, PULLED, TELEPORTED);
};

template <>
struct glz::meta<wlb::snapshot::Blank::Kind> {
  using enum wlb::snapshot::Blank::Kind;
  static constexpr auto value =
      glz::enumerate(KIND_UNSPECIFIED, TEXT, NEW_NAME, PLAYER, MONEY, COUNT);
};

template <>
struct glz::meta<wlb::snapshot::Option::Kind> {
  using enum wlb::snapshot::Option::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               ATTACK,
                                               TALK,
                                               LOOT,
                                               GATHER,
                                               USE,
                                               REST,
                                               EXPLORE,
                                               GO_TO,
                                               KEEP,
                                               INVITE,
                                               ACCEPT_INVITE,
                                               DECLINE_INVITE,
                                               LEAVE_GROUP);
};

template <>
struct glz::meta<wlb::snapshot::Relative::Height> {
  using enum wlb::snapshot::Relative::Height;
  static constexpr auto value =
      glz::enumerate(HEIGHT_UNSPECIFIED, LEVEL, ABOVE, BELOW);
};

template <>
struct glz::meta<wlb::snapshot::Spell::School> {
  using enum wlb::snapshot::Spell::School;
  static constexpr auto value = glz::enumerate(SCHOOL_UNSPECIFIED,
                                               PHYSICAL,
                                               HOLY,
                                               FIRE,
                                               NATURE,
                                               FROST,
                                               SHADOW,
                                               ARCANE);
};

template <>
struct glz::meta<wlb::snapshot::Item::Quality> {
  using enum wlb::snapshot::Item::Quality;
  static constexpr auto value = glz::enumerate(QUALITY_UNSPECIFIED,
                                               POOR,
                                               COMMON,
                                               UNCOMMON,
                                               RARE_QUALITY,
                                               EPIC,
                                               LEGENDARY,
                                               ARTIFACT,
                                               HEIRLOOM);
};

template <>
struct glz::meta<wlb::snapshot::Item::Kind> {
  using enum wlb::snapshot::Item::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               EQUIPMENT,
                                               CONSUMABLE,
                                               QUEST,
                                               TRADE_GOOD,
                                               RECIPE,
                                               CONTAINER,
                                               REAGENT,
                                               PROJECTILE,
                                               JUNK,
                                               KEY,
                                               GEM,
                                               GLYPH,
                                               CURRENCY_TOKEN,
                                               OTHER);
};

template <>
struct glz::meta<wlb::snapshot::Item::Binding> {
  using enum wlb::snapshot::Item::Binding;
  static constexpr auto value = glz::enumerate(BINDING_UNSPECIFIED,
                                               NOT_BOUND,
                                               SOULBOUND,
                                               BIND_ON_PICKUP,
                                               BIND_ON_EQUIP,
                                               BIND_ON_USE,
                                               BIND_TO_ACCOUNT);
};

template <>
struct glz::meta<wlb::snapshot::PetPart::Happiness> {
  using enum wlb::snapshot::PetPart::Happiness;
  static constexpr auto value =
      glz::enumerate(HAPPINESS_UNSPECIFIED, UNHAPPY, CONTENT, HAPPY);
};

template <>
struct glz::meta<wlb::snapshot::GameObject::Kind> {
  using enum wlb::snapshot::GameObject::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               QUEST_OBJECT,
                                               HERB,
                                               ORE,
                                               CHEST,
                                               MAILBOX,
                                               DOOR,
                                               CAMPFIRE,
                                               BOBBER,
                                               MEETING_STONE,
                                               SUMMONING_PORTAL,
                                               TRANSPORT,
                                               FISHING_POOL,
                                               BARBER_CHAIR,
                                               GUILD_VAULT,
                                               PORTAL,
                                               RITUAL,
                                               OTHER,
                                               CAPTURE_POINT,
                                               DESTRUCTIBLE);
};

template <>
struct glz::meta<wlb::snapshot::Place::Kind> {
  using enum wlb::snapshot::Place::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               TOWN,
                                               CAMP,
                                               FLIGHT_MASTER,
                                               INNKEEPER,
                                               GRAVEYARD,
                                               DUNGEON,
                                               RAID,
                                               ZONE_EXIT,
                                               LANDMARK,
                                               CAMPFIRE,
                                               AUCTION_HOUSE,
                                               BANK,
                                               TRAINER,
                                               VENDOR,
                                               DOCK,
                                               ZEPPELIN_TOWER,
                                               ZONE,
                                               OBJECTIVE_AREA,
                                               BATTLEMASTER,
                                               GUILD_BANK,
                                               BARBER_SHOP,
                                               STABLE);
};

template <>
struct glz::meta<wlb::snapshot::Surroundings_Obstacle::Kind> {
  using enum wlb::snapshot::Surroundings_Obstacle::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               WALL,
                                               SLOPE,
                                               DROP,
                                               WATER,
                                               LEDGE,
                                               OPENING,
                                               DOOR,
                                               TREE,
                                               ROCK);
};

template <>
struct glz::meta<wlb::snapshot::PromptEvent::Kind> {
  using enum wlb::snapshot::PromptEvent::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               GROUP_INVITE,
                                               TRADE,
                                               DUEL,
                                               GUILD_INVITE,
                                               READY_CHECK,
                                               ROLE_CHECK,
                                               RESURRECT_OFFER,
                                               SUMMON,
                                               QUEST_SHARE,
                                               BATTLEGROUND_READY,
                                               ARENA_TEAM_INVITE,
                                               WINTERGRASP_INVITE,
                                               BIND_HEARTHSTONE,
                                               CONFIRM_BIND_ON_PICKUP,
                                               CONFIRM_DELETE_ITEM,
                                               CONFIRM_ABANDON_QUEST,
                                               CONFIRM_REPLACE_ENCHANT,
                                               CONFIRM_TALENT_WIPE,
                                               CONFIRM_RELEASE_SPIRIT,
                                               CONFIRM_LOOT_ROLL,
                                               CONFIRM_PETITION_SIGN,
                                               CONFIRM_LEAVE_BATTLEGROUND,
                                               CONFIRM_INSTANCE_LOCK,
                                               CONFIRM_SPIRIT_HEALER,
                                               CONFIRM_GUILD_LEADER,
                                               CONFIRM_REPLACE_GEM,
                                               LFG_PROPOSAL,
                                               LFG_BOOT_VOTE,
                                               LFG_OFFER_CONTINUE,
                                               CONFIRM_PURCHASE,
                                               SPIRIT_HEALER_WAVE,
                                               RECLAIM_CORPSE,
                                               CHANNEL_INVITE,
                                               CHANNEL_PASSWORD,
                                               PET_NAME);
};

template <>
struct glz::meta<wlb::snapshot::Activity::State> {
  using enum wlb::snapshot::Activity::State;
  static constexpr auto value = glz::enumerate(STATE_UNSPECIFIED,
                                               IDLE,
                                               MOVING,
                                               PATROLLING,
                                               APPROACHING_ME,
                                               FLEEING,
                                               FIGHTING,
                                               CASTING,
                                               CHANNELING,
                                               TALKING,
                                               EMOTING,
                                               LOOTING,
                                               GATHERING,
                                               SITTING,
                                               FOLLOWING,
                                               FISHING,
                                               DEAD,
                                               DRINKING,
                                               STEALTHED,
                                               AFK);
};

template <>
struct glz::meta<wlb::snapshot::EquipmentPart::Slot> {
  using enum wlb::snapshot::EquipmentPart::Slot;
  static constexpr auto value = glz::enumerate(SLOT_UNSPECIFIED,
                                               HEAD,
                                               NECK,
                                               SHOULDER,
                                               BACK,
                                               CHEST,
                                               SHIRT,
                                               TABARD,
                                               WRIST,
                                               HANDS,
                                               WAIST,
                                               LEGS,
                                               FEET,
                                               FINGER1,
                                               FINGER2,
                                               TRINKET1,
                                               TRINKET2,
                                               MAIN_HAND,
                                               OFF_HAND,
                                               RANGED,
                                               BAG);
};

template <>
struct glz::meta<wlb::snapshot::Unit::Kind> {
  using enum wlb::snapshot::Unit::Kind;
  static constexpr auto value = glz::enumerate(KIND_UNSPECIFIED,
                                               NPC,
                                               PLAYER,
                                               PET,
                                               VEHICLE,
                                               TOTEM,
                                               CRITTER);
};

template <>
struct glz::meta<wlb::snapshot::Unit::LevelColor> {
  using enum wlb::snapshot::Unit::LevelColor;
  static constexpr auto value = glz::enumerate(LEVEL_COLOR_UNSPECIFIED,
                                               GRAY,
                                               GREEN,
                                               YELLOW,
                                               ORANGE,
                                               RED,
                                               SKULL_LEVEL);
};

template <>
struct glz::meta<wlb::snapshot::Unit::Reaction> {
  using enum wlb::snapshot::Unit::Reaction;
  static constexpr auto value =
      glz::enumerate(REACTION_UNSPECIFIED, HOSTILE, NEUTRAL, FRIENDLY);
};

template <>
struct glz::meta<wlb::snapshot::Unit::Classification> {
  using enum wlb::snapshot::Unit::Classification;
  static constexpr auto value = glz::enumerate(CLASSIFICATION_UNSPECIFIED,
                                               NORMAL,
                                               ELITE,
                                               RARE,
                                               RARE_ELITE,
                                               WORLD_BOSS);
};

template <>
struct glz::meta<wlb::snapshot::Unit::ThreatStatus> {
  using enum wlb::snapshot::Unit::ThreatStatus;
  static constexpr auto value = glz::enumerate(THREAT_STATUS_UNSPECIFIED,
                                               THREAT_NONE,
                                               THREAT_HIGH,
                                               THREAT_AGGRO);
};

template <>
struct glz::meta<wlb::snapshot::Unit::QuestMarker> {
  using enum wlb::snapshot::Unit::QuestMarker;
  static constexpr auto value = glz::enumerate(QUEST_MARKER_UNSPECIFIED,
                                               QUEST_AVAILABLE,
                                               QUEST_AVAILABLE_LOW,
                                               QUEST_TURN_IN,
                                               QUEST_INCOMPLETE,
                                               QUEST_DAILY);
};

template <>
struct glz::meta<wlb::snapshot::Unit::Service> {
  using enum wlb::snapshot::Unit::Service;
  static constexpr auto value = glz::enumerate(SERVICE_UNSPECIFIED,
                                               VENDOR,
                                               CLASS_TRAINER,
                                               PROFESSION_TRAINER,
                                               REPAIR,
                                               INNKEEPER,
                                               FLIGHT_MASTER,
                                               BANKER,
                                               AUCTIONEER,
                                               STABLE_MASTER,
                                               SPIRIT_HEALER,
                                               BATTLEMASTER,
                                               GUILD_MASTER,
                                               TABARD_VENDOR,
                                               QUEST_GIVER,
                                               GUILD_BANKER,
                                               BARBER,
                                               ARENA_ORGANIZER,
                                               WEAPON_MASTER);
};

template <>
struct glz::meta<wlb::snapshot::Unit::VisualState> {
  using enum wlb::snapshot::Unit::VisualState;
  static constexpr auto value = glz::enumerate(VISUAL_STATE_UNSPECIFIED,
                                               POLYMORPHED,
                                               STUNNED,
                                               FEARED,
                                               SAPPED,
                                               ASLEEP,
                                               BANISHED,
                                               FROZEN,
                                               ICE_BLOCK,
                                               BUBBLED,
                                               MIND_CONTROLLED,
                                               STEALTH_SHIMMER,
                                               SHACKLED,
                                               ROOTED,
                                               DRUNK_SWAY,
                                               FLYING,
                                               UNTARGETABLE,
                                               SUBMERGED);
};

template <>
struct glz::meta<wlb::snapshot::Unit::Size> {
  using enum wlb::snapshot::Unit::Size;
  static constexpr auto value =
      glz::enumerate(SIZE_UNSPECIFIED, SMALL, MEDIUM, LARGE, HUGE);
};

template <>
struct glz::meta<wlb::snapshot::Unit::Detail> {
  using enum wlb::snapshot::Unit::Detail;
  static constexpr auto value =
      glz::enumerate(DETAIL_UNSPECIFIED, NAMEPLATE, MODEL);
};

template <>
struct glz::meta<wlb::snapshot::Self::Gender> {
  using enum wlb::snapshot::Self::Gender;
  static constexpr auto value =
      glz::enumerate(GENDER_UNSPECIFIED, MALE, FEMALE);
};

template <>
struct glz::meta<wlb::snapshot::Self::PowerType> {
  using enum wlb::snapshot::Self::PowerType;
  static constexpr auto value = glz::enumerate(POWER_TYPE_UNSPECIFIED,
                                               MANA,
                                               RAGE,
                                               ENERGY,
                                               RUNIC_POWER,
                                               FOCUS);
};

template <>
struct glz::meta<wlb::snapshot::Self::LifeState> {
  using enum wlb::snapshot::Self::LifeState;
  static constexpr auto value =
      glz::enumerate(LIFE_STATE_UNSPECIFIED, ALIVE, DEAD_BODY, GHOST);
};

template <>
struct glz::meta<wlb::snapshot::Self::Drunk> {
  using enum wlb::snapshot::Self::Drunk;
  static constexpr auto value =
      glz::enumerate(DRUNK_UNSPECIFIED, SOBER, TIPSY, DRUNK, SMASHED);
};

// How every struct of snapshot.h goes on the wire and into JSON. Enums are
// listed above (their names are the wire format); structs need no list:
// Glaze reflects their fields, and the one specialization below gives them
// all the same two rules.
namespace wlb::snapshot::wire_detail {

// True for a field left at its default: an unset optional or Box, an empty
// string or list, or a value equal to its default (0, false, an UNSPECIFIED
// enum, a struct with nothing set). Such fields are not written.
template <typename V>
bool IsDefaultValue(const V& v) {
  if constexpr (requires { v.has_value(); }) {
    return !v.has_value();
  } else if constexpr (requires { v.empty(); }) {
    return v.empty();
  } else {
    return v == V{};
  }
}

// The structs declared directly in namespace wlb::snapshot: not templates
// (Box), not std types holding ours, not this file's helpers in nested
// namespaces.
template <typename T>
consteval bool IsSnapshotStruct() {
  constexpr std::string_view kNamespace = "wlb::snapshot::";
  constexpr std::string_view name = glz::type_name<T>;
  return name.starts_with(kNamespace) &&
         name.find_first_of(":<", kNamespace.size()) == std::string_view::npos;
}

template <typename T>
concept SnapshotStruct = std::is_aggregate_v<T> && IsSnapshotStruct<T>();

}  // namespace wlb::snapshot::wire_detail

// Fields at their default are left out, and a field named after a C++
// keyword (`class_`, `requires_`) is written without its underscore.
template <typename T>
  requires wlb::snapshot::wire_detail::SnapshotStruct<T>
struct glz::meta<T> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
  static constexpr auto rename_key = [](std::string_view key) {
    return key.ends_with('_') ? key.substr(0, key.size() - 1) : key;
  };
};

namespace wlb::snapshot {
namespace {

// Read limits: a frame is at most 1 MiB (net/frame_codec.h), but a corrupt one
// can claim a huge length for a string or array; refuse it before allocating.
// Far above anything real: a snapshot's longest strings are chat lines and
// tooltips, its longest arrays the nearby units of a crowded city (~200).
inline constexpr size_t kMaxWireStringBytes = 64 * 1024;
inline constexpr size_t kMaxWireArraySize = 16 * 1024;

struct WireBeveOpts : glz::opts {
  uint32_t format = glz::BEVE;
  bool skip_null_members = true;
  bool skip_default_members = true;
  bool error_on_unknown_keys = false;
  size_t max_string_length = kMaxWireStringBytes;
  size_t max_array_size = kMaxWireArraySize;
};

struct StrictJsonReadOpts : glz::opts {
  uint32_t format = glz::JSON;
  bool error_on_unknown_keys = true;
  size_t max_string_length = kMaxWireStringBytes;
  size_t max_array_size = kMaxWireArraySize;
};

struct CompactJsonWriteOpts : glz::opts {
  uint32_t format = glz::JSON;
  bool skip_null_members = true;
  bool skip_default_members = true;
};

struct PrettyJsonWriteOpts : glz::opts {
  uint32_t format = glz::JSON;
  bool skip_null_members = true;
  bool skip_default_members = true;
  bool prettify = true;
  uint8_t indentation_width = 2;
};

template <typename T>
std::string WriteBeveOf(const T& value) {
  std::string out;
  (void)glz::write<WireBeveOpts{}>(value, out);
  return out;
}

template <typename T>
bool ReadBeveOf(std::string_view bytes, T* out) {
  *out = T{};
  return !glz::read<WireBeveOpts{}>(*out, bytes);
}

template <typename T>
std::string WriteJsonOf(const T& value, bool pretty) {
  std::string out;
  // An enum value with no name (a newer peer's, or memory gone bad) makes the
  // writer fail half way, leaving invalid JSON: return nothing instead.
  const auto ec = pretty ? glz::write<PrettyJsonWriteOpts{}>(value, out)
                         : glz::write<CompactJsonWriteOpts{}>(value, out);
  if (ec) {
    return "";
  }
  return out;
}

template <typename T>
bool ReadJsonOf(std::string_view json, T* out, std::string* error) {
  *out = T{};
  const auto ec = glz::read<StrictJsonReadOpts{}>(*out, json);
  if (ec) {
    if (error != nullptr) {
      *error = glz::format_error(ec, json);
    }
    return false;
  }
  return true;
}

// Whether a field would be written (wire_detail::IsDefaultValue).
template <typename T>
bool IsFieldPresent(const T& v) {
  return !wire_detail::IsDefaultValue(v);
}

// The hover-only fields (`visibility: HOVER` in snapshot.h), as
// "Struct.field": shown only inside base.tooltips. The allow-list the walker
// below checks every field against; `tools/wlb check snapshots` fails when it
// differs from the tags in snapshot.h, so a new tag can't go unchecked.
inline constexpr std::string_view kHoverFields[] = {
    "PlayerPart.race",
    "PlayerPart.guild",
    "PlayerPart.gearscore",
    "Spell.school",
    "Spell.cost",
    "Spell.cast_time_ms",
    "Spell.range_yd",
    "Spell.tooltip",
    "Aura.tooltip",
    "Item.item_level",
    "Item.required_level",
    "Item.binding",
    "Item.unique",
    "Item.vendor_value",
    "Item.tooltip",
    "Place.level_range",
    "EquipmentPart.stats",
    "EquipmentPart.sockets",
    "EquipmentPart.socket_bonus",
    "EquipmentPart.enchant",
    "EquipmentPart.type",
    "EquipmentPart.gearscore",
    "EquipmentPart.gearscore_delta",
    "EquipmentPart.upgrade",
    "Unit.services",
    "Base.tooltips",
};

bool IsHoverField(std::string_view type, std::string_view key) {
  for (const std::string_view field : kHoverFields) {
    const size_t dot = field.find('.');
    if (field.substr(0, dot) == type && field.substr(dot + 1) == key) {
      return true;
    }
  }
  return false;
}

constexpr std::string_view ShortTypeName(std::string_view full) {
  const size_t colons = full.rfind("::");
  return colons == std::string_view::npos ? full : full.substr(colons + 2);
}

template <typename T>
struct IsVectorOf : std::false_type {};
template <typename U, typename A>
struct IsVectorOf<std::vector<U, A>> : std::true_type {};
template <typename T>
struct IsPair : std::false_type {};
template <typename A, typename B>
struct IsPair<std::pair<A, B>> : std::true_type {};

// Walks every field of every message by reflection (review of f40b0b6,
// finding 17: the hand-written walker missed fields added after it), so a
// new field or struct is covered with no edit here.
template <typename T>
void WalkValue(const T& v,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors);

template <typename T>
void WalkFields(const T& v,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  constexpr std::string_view type = ShortTypeName(glz::name_v<T>);
  // Glaze ties members of a non-const object; nothing is written.
  T& object = const_cast<T&>(v);
  [&]<size_t... Is>(std::index_sequence<Is...>) {
    (
        [&] {
          constexpr std::string_view key = glz::reflect<T>::keys[Is];
          const auto& member =
              glz::get_member(object, glz::get<Is>(glz::to_tie(object)));
          if (!IsFieldPresent(member)) {
            return;
          }
          const std::string here = base::StrCat({path, ".", key});
          // base.tooltips is the tooltip area itself.
          const bool tooltips = type == "Base" && key == "tooltips";
          if (!in_tooltip && !tooltips && IsHoverField(type, key)) {
            errors->push_back(base::StrCat(
                {here, ": hover-only field outside base.tooltips"}));
          }
          WalkValue(member, here, in_tooltip || tooltips, errors);
        }(),
        ...);
  }(std::make_index_sequence<glz::reflect<T>::size>{});
}

template <typename T>
void WalkValue(const T& v,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if constexpr (requires {
                  v.has_value();
                  v.value();
                }) {  // std::optional, Box.
    if (v.has_value()) {
      WalkValue(v.value(), path, in_tooltip, errors);
    }
  } else if constexpr (IsVectorOf<T>::value) {
    for (size_t i = 0; i < v.size(); ++i) {
      WalkValue(v[i], base::StrCat({path, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  } else if constexpr (!IsPair<T>::value &&
                       (glz::reflectable<T> || glz::glaze_object_t<T>)) {
    WalkFields(v, path, in_tooltip, errors);
  }
}

}  // namespace

std::vector<std::string> ValidateSnapshotVisibility(const Snapshot& snap) {
  std::vector<std::string> errors;
  WalkValue(snap, "$", false, &errors);
  // A tooltip shows for what the bot points at, nothing else.
  for (size_t i = 0; i < snap.base.tooltips.size(); ++i) {
    const Tooltipped& t = snap.base.tooltips[i];
    if (t.ref &&
        std::none_of(snap.base.ui.hovering.begin(), snap.base.ui.hovering.end(),
                     [&](const HoverRef& h) {
                       return h.kind == t.ref->kind && h.id == t.ref->id;
                     })) {
      errors.push_back(
          base::StrCat({"$.base.tooltips[", base::NumberToString(i),
                        "]: not for anything in base.ui.hovering"}));
    }
  }
  for (size_t i = 0; i < snap.base.nearby.units.size(); ++i) {
    const Unit& u = snap.base.nearby.units[i];
    if (u.detail == Unit::MODEL) {
      std::vector<std::string_view> extra;
      if (IsFieldPresent(u.name))
        extra.push_back("name");
      if (IsFieldPresent(u.level))
        extra.push_back("level");
      if (IsFieldPresent(u.level_color))
        extra.push_back("level_color");
      if (IsFieldPresent(u.reaction))
        extra.push_back("reaction");
      if (IsFieldPresent(u.classification))
        extra.push_back("classification");
      if (IsFieldPresent(u.health_pct))
        extra.push_back("health_pct");
      if (IsFieldPresent(u.power_pct))
        extra.push_back("power_pct");
      if (IsFieldPresent(u.skinnable))
        extra.push_back("skinnable");
      if (IsFieldPresent(u.tapped_by_other))
        extra.push_back("tapped_by_other");
      if (IsFieldPresent(u.targeting_me))
        extra.push_back("targeting_me");
      if (IsFieldPresent(u.target))
        extra.push_back("target");
      if (IsFieldPresent(u.threat_status))
        extra.push_back("threat_status");
      if (IsFieldPresent(u.quest_marker))
        extra.push_back("quest_marker");
      if (IsFieldPresent(u.pvp_flagged))
        extra.push_back("pvp_flagged");
      if (IsFieldPresent(u.auras))
        extra.push_back("auras");
      if (IsFieldPresent(u.quest_relevant))
        extra.push_back("quest_relevant");
      if (IsFieldPresent(u.services))
        extra.push_back("services");
      if (IsFieldPresent(u.subtitle))
        extra.push_back("subtitle");
      if (IsFieldPresent(u.player))
        extra.push_back("player");
      if (IsFieldPresent(u.group))
        extra.push_back("group");
      if (IsFieldPresent(u.boss))
        extra.push_back("boss");
      if (IsFieldPresent(u.pet))
        extra.push_back("pet");
      if (IsFieldPresent(u.vehicle))
        extra.push_back("vehicle");
      if (!extra.empty()) {
        std::sort(extra.begin(), extra.end());
        std::string joined(extra[0]);
        for (size_t j = 1; j < extra.size(); ++j) {
          base::StrAppend(&joined, {", ", extra[j]});
        }
        errors.push_back(
            base::StrCat({"$.base.nearby.units[", base::NumberToString(i),
                          "]: MODEL detail but shows ", joined}));
      }
    }
  }
  const auto has_panel = [&](Panel p) {
    return std::find(snap.base.ui.open_panels.begin(),
                     snap.base.ui.open_panels.end(),
                     p) != snap.base.ui.open_panels.end();
  };
  if (snap.ext.bags.has_value() && !has_panel(Panel::BAGS)) {
    errors.push_back(
        "$.ext.bags: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.map.has_value() && !has_panel(Panel::MAP)) {
    errors.push_back("$.ext.map: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.character.has_value() && !has_panel(Panel::CHARACTER)) {
    errors.push_back(
        "$.ext.character: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.quest_log.has_value() && !has_panel(Panel::QUEST_LOG)) {
    errors.push_back(
        "$.ext.quest_log: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.spellbook.has_value() && !has_panel(Panel::SPELLBOOK)) {
    errors.push_back(
        "$.ext.spellbook: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.talents.has_value() && !has_panel(Panel::TALENTS)) {
    errors.push_back(
        "$.ext.talents: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.skills.has_value() && !has_panel(Panel::SKILLS)) {
    errors.push_back(
        "$.ext.skills: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.reputation.has_value() && !has_panel(Panel::REPUTATION)) {
    errors.push_back(
        "$.ext.reputation: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.social.has_value() && !has_panel(Panel::SOCIAL)) {
    errors.push_back(
        "$.ext.social: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.calendar.has_value() && !has_panel(Panel::CALENDAR)) {
    errors.push_back(
        "$.ext.calendar: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.achievements.has_value() && !has_panel(Panel::ACHIEVEMENTS)) {
    errors.push_back(
        "$.ext.achievements: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.pvp.has_value() && !has_panel(Panel::PVP)) {
    errors.push_back("$.ext.pvp: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.companions.has_value() && !has_panel(Panel::COMPANIONS)) {
    errors.push_back(
        "$.ext.companions: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.dungeon_finder.has_value() &&
      !has_panel(Panel::DUNGEON_FINDER)) {
    errors.push_back(
        "$.ext.dungeon_finder: panel present but not in base.ui.open_panels");
  }
  if (snap.ext.scoreboard.has_value() && !has_panel(Panel::SCOREBOARD)) {
    errors.push_back(
        "$.ext.scoreboard: panel present but not in base.ui.open_panels");
  }
  return errors;
}

namespace detail {

template <typename T>
struct UnwrapFieldType {
  using type = T;
};
template <typename T>
struct UnwrapFieldType<std::optional<T>> {
  using type = T;
};
template <typename T>
struct UnwrapFieldType<std::vector<T>> {
  using type = T;
};
template <typename T>
struct UnwrapFieldType<Box<T>> {
  using type = T;
};

template <typename T, int Depth>
void CollectFieldPaths(std::string_view prefix, std::vector<std::string>* out) {
  if constexpr (glz::reflectable<T> || glz::glaze_object_t<T>) {
    constexpr auto N = glz::reflect<T>::size;
    [&]<size_t... Is>(std::index_sequence<Is...>) {
      (([&] {
         constexpr std::string_view key = glz::reflect<T>::keys[Is];
         const std::string path = base::StrCat({prefix, key});
         out->push_back(path);
         if constexpr (Depth > 0) {
           using RawMember = std::remove_cvref_t<decltype(glz::get_member(
               std::declval<T&>(),
               glz::get<Is>(glz::to_tie(std::declval<T&>()))))>;
           using Sub = typename UnwrapFieldType<RawMember>::type;
           if constexpr ((glz::reflectable<Sub> || glz::glaze_object_t<Sub>) &&
                         !std::is_same_v<Sub,
                                         std::pair<std::string, std::string>> &&
                         !std::is_same_v<Sub,
                                         std::pair<std::string, int32_t>> &&
                         !std::is_same_v<Sub,
                                         std::pair<std::string, uint32_t>>) {
             CollectFieldPaths<Sub, Depth - 1>(base::StrCat({path, "."}), out);
           }
         }
       }()),
       ...);
    }(std::make_index_sequence<N>{});
  }
}

struct SchemaDump {
  std::vector<std::string> events;
  std::vector<std::string> prompts;
  std::vector<std::string> fields;
};

}  // namespace detail

std::string ReflectedSchemaJson() {
  detail::SchemaDump dump;
  for (std::string_view k : glz::reflect<Event>::keys) {
    if (k != "prompt") {
      dump.events.push_back(base::StrCat({"event.", k}));
    }
  }
  constexpr auto& prompt_keys = glz::reflect<PromptEvent::Kind>::keys;
  for (size_t i = 1; i < prompt_keys.size(); ++i) {
    dump.prompts.push_back(base::StrCat({"prompt.", prompt_keys[i]}));
  }
  detail::CollectFieldPaths<Snapshot, 4>("", &dump.fields);
  std::sort(dump.fields.begin(), dump.fields.end());
  dump.fields.erase(std::unique(dump.fields.begin(), dump.fields.end()),
                    dump.fields.end());
  std::string out;
  (void)glz::write_json(dump, out);
  return out;
}

std::string WriteBeve(const ModuleMessage& msg) {
  return WriteBeveOf(msg);
}
bool ReadBeve(std::string_view bytes, ModuleMessage* out) {
  return ReadBeveOf(bytes, out);
}
std::string WriteBeve(const Command& cmd) {
  return WriteBeveOf(cmd);
}
bool ReadBeve(std::string_view bytes, Command* out) {
  return ReadBeveOf(bytes, out);
}
std::string WriteBeve(const Snapshot& snap) {
  return WriteBeveOf(snap);
}
bool ReadBeve(std::string_view bytes, Snapshot* out) {
  return ReadBeveOf(bytes, out);
}

std::string WriteJson(const Snapshot& snap, bool pretty) {
  return WriteJsonOf(snap, pretty);
}
bool ReadJson(std::string_view json, Snapshot* out, std::string* error) {
  return ReadJsonOf(json, out, error);
}
std::string WriteJson(const ModuleMessage& msg, bool pretty) {
  return WriteJsonOf(msg, pretty);
}
bool ReadJson(std::string_view json, ModuleMessage* out, std::string* error) {
  return ReadJsonOf(json, out, error);
}
std::string WriteJson(const Command& cmd, bool pretty) {
  return WriteJsonOf(cmd, pretty);
}
bool ReadJson(std::string_view json, Command* out, std::string* error) {
  return ReadJsonOf(json, out, error);
}
std::string WriteJson(const OptionFacts& facts, bool pretty) {
  return WriteJsonOf(facts, pretty);
}
bool ReadJson(std::string_view json, OptionFacts* out, std::string* error) {
  return ReadJsonOf(json, out, error);
}

std::string_view EnumName(Direction value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(DispelType value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(RaidMarker value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Role value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Stat value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Panel value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(ChatChannel value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Self_Indicators_Durability::Level value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(GoalPart::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(HoverRef::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Calendar_Entry::Status value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(DisplacedEvent::How value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Blank::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Command_Act::Verb value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Command_Act::PlaceKind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Command_Act::ThingKind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Command_Act::StopKind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Command_Act::PetCmd value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Relative::Height value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Spell::School value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Item::Quality value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Item::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Item::Binding value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(PetPart::Happiness value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(GameObject::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Place::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Surroundings_Obstacle::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(PromptEvent::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Activity::State value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(EquipmentPart::Slot value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::Kind value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::LevelColor value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::Reaction value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::Classification value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::ThreatStatus value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::QuestMarker value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::Service value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::VisualState value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::Size value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Unit::Detail value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Self::Gender value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Self::PowerType value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Self::LifeState value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Self::Drunk value) {
  return glz::get_enum_name(value);
}

std::string_view EnumName(Option::Kind value) {
  return glz::get_enum_name(value);
}

}  // namespace wlb::snapshot
