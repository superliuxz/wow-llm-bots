// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "proto/snapshot.h"

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

// Glaze string-enum and C++ keyword field mappings.

namespace wlb::snapshot::wire_detail {
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
}  // namespace wlb::snapshot::wire_detail

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
                                               DRUNK_SWAY);
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

template <>
struct glz::meta<wlb::snapshot::TrainerSpell> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
  using T = wlb::snapshot::TrainerSpell;
  static constexpr auto value = glz::object("price",
                                            &T::price,
                                            "required_level",
                                            &T::required_level,
                                            "learnable",
                                            &T::learnable,
                                            "learned",
                                            &T::learned,
                                            "requires",
                                            &T::requires_);
};

template <>
struct glz::meta<wlb::snapshot::PlayerPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
  using T = wlb::snapshot::PlayerPart;
  static constexpr auto value = glz::object("class",
                                            &T::class_,
                                            "race",
                                            &T::race,
                                            "guild",
                                            &T::guild,
                                            "title",
                                            &T::title,
                                            "in_my_group",
                                            &T::in_my_group,
                                            "is_friend",
                                            &T::is_friend,
                                            "is_ignored",
                                            &T::is_ignored,
                                            "afk",
                                            &T::afk,
                                            "dnd",
                                            &T::dnd,
                                            "gearscore",
                                            &T::gearscore);
};

template <>
struct glz::meta<wlb::snapshot::Scoreboard_Row> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
  using T = wlb::snapshot::Scoreboard_Row;
  static constexpr auto value = glz::object("name",
                                            &T::name,
                                            "class",
                                            &T::class_,
                                            "team",
                                            &T::team,
                                            "killing_blows",
                                            &T::killing_blows,
                                            "honorable_kills",
                                            &T::honorable_kills,
                                            "deaths",
                                            &T::deaths,
                                            "bonus_honor",
                                            &T::bonus_honor,
                                            "damage_done",
                                            &T::damage_done,
                                            "healing_done",
                                            &T::healing_done,
                                            "objectives",
                                            &T::objectives,
                                            "rating_change",
                                            &T::rating_change);
};

template <>
struct glz::meta<wlb::snapshot::Social_Friend> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
  using T = wlb::snapshot::Social_Friend;
  static constexpr auto value = glz::object("name",
                                            &T::name,
                                            "online",
                                            &T::online,
                                            "level",
                                            &T::level,
                                            "class",
                                            &T::class_,
                                            "zone",
                                            &T::zone,
                                            "note",
                                            &T::note);
};

template <>
struct glz::meta<wlb::snapshot::Vec3> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::LevelRange> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Tooltip> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::CurrencyAmount> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::StatValue> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Spell_Cost> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::AbilityState> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Unit_Pack> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::GroupPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::BossPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::VehiclePart_Seat> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self_Control> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self_Indicators_Durability> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self_AutoAttack> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self_WeaponEnchant> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self_Rune> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Objective_Progress> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::GoalPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::AchievementPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Achievement_Criterion> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Doing> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::HoverRef> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::WorldState> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::World_Queue> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Combat_Threat> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Combat_Resources> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Encounter_Timer> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Instance_Boss> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Gossip_Option> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_RaidBrowserEntry> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Barber> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Petition> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Registrar> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_ItemText> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Tabard> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Battlemaster> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::PetFrame_Totem> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Environment_Falling> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Battleground_Objective> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Bags_Hearthstone> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Character_EquipmentSet> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Talents_Talent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Talents_Glyph> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Talents_Spec> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Skills_Skill> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Reputation_Faction> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Calendar_Entry> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DungeonFinder_Queue> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Pvp_ArenaTeam> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ErrorEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::InfoEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::LevelUpEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::XpGainedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::MoneyChangedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::SkillUpEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::QuestEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DiedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ResurrectedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::RollEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::MailEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ZoneEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::InstanceEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::CalendarAlarmEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DisplacedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DuelEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::TitleEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::KickedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::HearthstoneBoundEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DbmWarningEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::CinematicEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::TalentPointEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Blank> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_Choose> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_ClearGoal> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_Say> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_Open> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_Close> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_AhSearch> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_Who> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Relative> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Spell> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Aura> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self_Indicators> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Item> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Achievement> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Where> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Ui> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::World> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Gossip> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_RaidBrowser> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Environment> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Battleground> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Scoreboard> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Map_Zone> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Talents> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Skills> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Reputation> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Social_Guild> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Social_WhoResult> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Calendar> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DungeonFinder_Dungeon> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Pvp> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Option> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_SetGoal> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command_Hover> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Cast> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::PetPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::VehiclePart_Aim> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::GameObject> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Area> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Place> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::EquipmentPart_Socket> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::QuestPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ActionBars> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Merchant> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_QuestDialog> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Mail> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_AuctionHouse> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Recipe_Reagent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Bank> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_GuildBank_Tab> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Trainer> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::TradeLoot_Trade> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::TradeLoot_Roll> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Surroundings_Obstacle> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Bags> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Character> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Companions> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Spellbook> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Social> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Achievements> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::DungeonFinder> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::PromptEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ItemReceivedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::LootRollEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::LootWonEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::LootRollChoiceEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::AuctionEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::MinimapPingEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::AchievementEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::BattlegroundEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::LfgEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Command> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Activity> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::VehiclePart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::EquipmentPart> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Objective> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Instance> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Death> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_FlightMap_Destination> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Recipe> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_GuildBank> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Socketing> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::MailInbox> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::TradeLoot> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Travel_Transport> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Surroundings> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Map_Questie> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Map_InstanceMap> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ChatLink> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::PathFailedEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Unit> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_FlightMap> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Tradeskill> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Travel> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Map> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::QuestLog_Entry> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ChatEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Self> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Nearby> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Tooltipped> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Minimap> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Target> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Combat> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Party> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Raid> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Encounter> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Inspect> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction_Stable> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::PetFrame> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Arena> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::QuestLog> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Event> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Base> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Interaction> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ImmediateEvent> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Extensions> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::Snapshot> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
      };
};

template <>
struct glz::meta<wlb::snapshot::ModuleMessage> {
  static constexpr auto skip_if =
      [](const auto& v, std::string_view, glz::meta_context) {
        return wlb::snapshot::wire_detail::IsDefaultValue(v);
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
bool IsFieldPresent(const T& v) {
  if constexpr (requires { v.has_value(); }) {
    return v.has_value();
  } else if constexpr (requires { v.empty(); }) {
    return !v.empty();
  } else {
    return !(v == T{});
  }
}

[[maybe_unused]] void Walk_Vec3(const Vec3& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_LevelRange(const LevelRange& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Tooltip(const Tooltip& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors);
[[maybe_unused]] void Walk_CurrencyAmount(const CurrencyAmount& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_StatValue(const StatValue& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Spell_Cost(const Spell_Cost& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_AbilityState(const AbilityState& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_TrainerSpell(const TrainerSpell& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Unit_Pack(const Unit_Pack& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_PlayerPart(const PlayerPart& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_GroupPart(const GroupPart& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_BossPart(const BossPart& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_VehiclePart_Seat(const VehiclePart_Seat& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self_Control(const Self_Control& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self_Indicators_Durability(
    const Self_Indicators_Durability& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self_AutoAttack(const Self_AutoAttack& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self_WeaponEnchant(const Self_WeaponEnchant& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self_Rune(const Self_Rune& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Objective_Progress(const Objective_Progress& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_GoalPart(const GoalPart& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_AchievementPart(const AchievementPart& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Achievement_Criterion(
    const Achievement_Criterion& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Doing(const Doing& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_HoverRef(const HoverRef& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_WorldState(const WorldState& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_World_Queue(const World_Queue& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Combat_Threat(const Combat_Threat& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Combat_Resources(const Combat_Resources& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Encounter_Timer(const Encounter_Timer& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Instance_Boss(const Instance_Boss& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Gossip_Option(
    const Interaction_Gossip_Option& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_RaidBrowserEntry(
    const Interaction_RaidBrowserEntry& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Barber(const Interaction_Barber& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Petition(
    const Interaction_Petition& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Registrar(
    const Interaction_Registrar& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_ItemText(
    const Interaction_ItemText& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Tabard(const Interaction_Tabard& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Battlemaster(
    const Interaction_Battlemaster& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_PetFrame_Totem(const PetFrame_Totem& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Environment_Falling(
    const Environment_Falling& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Battleground_Objective(
    const Battleground_Objective& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Scoreboard_Row(const Scoreboard_Row& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Bags_Hearthstone(const Bags_Hearthstone& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Character_EquipmentSet(
    const Character_EquipmentSet& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Talents_Talent(const Talents_Talent& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Talents_Glyph(const Talents_Glyph& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Talents_Spec(const Talents_Spec& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Skills_Skill(const Skills_Skill& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Reputation_Faction(const Reputation_Faction& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Social_Friend(const Social_Friend& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Calendar_Entry(const Calendar_Entry& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DungeonFinder_Queue(
    const DungeonFinder_Queue& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Pvp_ArenaTeam(const Pvp_ArenaTeam& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ErrorEvent(const ErrorEvent& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_InfoEvent(const InfoEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_LevelUpEvent(const LevelUpEvent& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_XpGainedEvent(const XpGainedEvent& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_MoneyChangedEvent(const MoneyChangedEvent& msg,
                                             std::string_view path,
                                             bool in_tooltip,
                                             std::vector<std::string>* errors);
[[maybe_unused]] void Walk_SkillUpEvent(const SkillUpEvent& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_QuestEvent(const QuestEvent& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DiedEvent(const DiedEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ResurrectedEvent(const ResurrectedEvent& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_RollEvent(const RollEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_MailEvent(const MailEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ZoneEvent(const ZoneEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_InstanceEvent(const InstanceEvent& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_CalendarAlarmEvent(const CalendarAlarmEvent& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DisplacedEvent(const DisplacedEvent& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DuelEvent(const DuelEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_TitleEvent(const TitleEvent& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_KickedEvent(const KickedEvent& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_HearthstoneBoundEvent(
    const HearthstoneBoundEvent& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DbmWarningEvent(const DbmWarningEvent& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_CinematicEvent(const CinematicEvent& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_TalentPointEvent(const TalentPointEvent& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Blank(const Blank& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_Choose(const Command_Choose& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_ClearGoal(const Command_ClearGoal& msg,
                                             std::string_view path,
                                             bool in_tooltip,
                                             std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_Say(const Command_Say& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_Open(const Command_Open& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_Close(const Command_Close& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_AhSearch(const Command_AhSearch& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_Who(const Command_Who& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Relative(const Relative& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Spell(const Spell& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Aura(const Aura& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self_Indicators(const Self_Indicators& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Item(const Item& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Achievement(const Achievement& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Where(const Where& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Ui(const Ui& msg,
                              std::string_view path,
                              bool in_tooltip,
                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_World(const World& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Gossip(const Interaction_Gossip& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_RaidBrowser(
    const Interaction_RaidBrowser& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Environment(const Environment& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Battleground(const Battleground& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Scoreboard(const Scoreboard& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Map_Zone(const Map_Zone& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Talents(const Talents& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Skills(const Skills& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Reputation(const Reputation& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Social_Guild(const Social_Guild& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Social_WhoResult(const Social_WhoResult& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Calendar(const Calendar& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DungeonFinder_Dungeon(
    const DungeonFinder_Dungeon& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Pvp(const Pvp& msg,
                               std::string_view path,
                               bool in_tooltip,
                               std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Option(const Option& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_SetGoal(const Command_SetGoal& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command_Hover(const Command_Hover& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Cast(const Cast& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_PetPart(const PetPart& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors);
[[maybe_unused]] void Walk_VehiclePart_Aim(const VehiclePart_Aim& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_GameObject(const GameObject& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Area(const Area& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Place(const Place& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_EquipmentPart_Socket(
    const EquipmentPart_Socket& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_QuestPart(const QuestPart& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ActionBars(const ActionBars& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Merchant(
    const Interaction_Merchant& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_QuestDialog(
    const Interaction_QuestDialog& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Mail(const Interaction_Mail& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_AuctionHouse(
    const Interaction_AuctionHouse& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Recipe_Reagent(
    const Interaction_Recipe_Reagent& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Bank(const Interaction_Bank& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_GuildBank_Tab(
    const Interaction_GuildBank_Tab& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Trainer(
    const Interaction_Trainer& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_TradeLoot_Trade(const TradeLoot_Trade& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_TradeLoot_Roll(const TradeLoot_Roll& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Surroundings_Obstacle(
    const Surroundings_Obstacle& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Bags(const Bags& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Character(const Character& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Companions(const Companions& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Spellbook(const Spellbook& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Social(const Social& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Achievements(const Achievements& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_DungeonFinder(const DungeonFinder& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_PromptEvent(const PromptEvent& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ItemReceivedEvent(const ItemReceivedEvent& msg,
                                             std::string_view path,
                                             bool in_tooltip,
                                             std::vector<std::string>* errors);
[[maybe_unused]] void Walk_LootRollEvent(const LootRollEvent& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_LootWonEvent(const LootWonEvent& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_LootRollChoiceEvent(
    const LootRollChoiceEvent& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_AuctionEvent(const AuctionEvent& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_MinimapPingEvent(const MinimapPingEvent& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_AchievementEvent(const AchievementEvent& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_BattlegroundEvent(const BattlegroundEvent& msg,
                                             std::string_view path,
                                             bool in_tooltip,
                                             std::vector<std::string>* errors);
[[maybe_unused]] void Walk_LfgEvent(const LfgEvent& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Command(const Command& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Activity(const Activity& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_VehiclePart(const VehiclePart& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_EquipmentPart(const EquipmentPart& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Objective(const Objective& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Instance(const Instance& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Death(const Death& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_FlightMap_Destination(
    const Interaction_FlightMap_Destination& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Recipe(const Interaction_Recipe& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_GuildBank(
    const Interaction_GuildBank& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Socketing(
    const Interaction_Socketing& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_MailInbox(const MailInbox& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_TradeLoot(const TradeLoot& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Travel_Transport(const Travel_Transport& msg,
                                            std::string_view path,
                                            bool in_tooltip,
                                            std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Surroundings(const Surroundings& msg,
                                        std::string_view path,
                                        bool in_tooltip,
                                        std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Map_Questie(const Map_Questie& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Map_InstanceMap(const Map_InstanceMap& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ChatLink(const ChatLink& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_PathFailedEvent(const PathFailedEvent& msg,
                                           std::string_view path,
                                           bool in_tooltip,
                                           std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Unit(const Unit& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_FlightMap(
    const Interaction_FlightMap& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Tradeskill(
    const Interaction_Tradeskill& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Travel(const Travel& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Map(const Map& msg,
                               std::string_view path,
                               bool in_tooltip,
                               std::vector<std::string>* errors);
[[maybe_unused]] void Walk_QuestLog_Entry(const QuestLog_Entry& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ChatEvent(const ChatEvent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Self(const Self& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Nearby(const Nearby& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Tooltipped(const Tooltipped& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Minimap(const Minimap& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Target(const Target& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Combat(const Combat& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Party(const Party& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Raid(const Raid& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Encounter(const Encounter& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Inspect(
    const Interaction_Inspect& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction_Stable(const Interaction_Stable& msg,
                                              std::string_view path,
                                              bool in_tooltip,
                                              std::vector<std::string>* errors);
[[maybe_unused]] void Walk_PetFrame(const PetFrame& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Arena(const Arena& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_QuestLog(const QuestLog& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Event(const Event& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Base(const Base& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Interaction(const Interaction& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ImmediateEvent(const ImmediateEvent& msg,
                                          std::string_view path,
                                          bool in_tooltip,
                                          std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Extensions(const Extensions& msg,
                                      std::string_view path,
                                      bool in_tooltip,
                                      std::vector<std::string>* errors);
[[maybe_unused]] void Walk_Snapshot(const Snapshot& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors);
[[maybe_unused]] void Walk_ModuleMessage(const ModuleMessage& msg,
                                         std::string_view path,
                                         bool in_tooltip,
                                         std::vector<std::string>* errors);

void Walk_Vec3(const Vec3& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_LevelRange(const LevelRange& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Tooltip(const Tooltip& msg,
                  std::string_view path,
                  bool in_tooltip,
                  std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_CurrencyAmount(const CurrencyAmount& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_StatValue(const StatValue& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Spell_Cost(const Spell_Cost& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_AbilityState(const AbilityState& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_TrainerSpell(const TrainerSpell& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Unit_Pack(const Unit_Pack& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_PlayerPart(const PlayerPart& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.race)) {
    const std::string here = base::StrCat({path, ".race"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.guild)) {
    const std::string here = base::StrCat({path, ".guild"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.gearscore)) {
    const std::string here = base::StrCat({path, ".gearscore"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
}

void Walk_GroupPart(const GroupPart& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_BossPart(const BossPart& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_VehiclePart_Seat(const VehiclePart_Seat& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Self_Control(const Self_Control& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Self_Indicators_Durability(const Self_Indicators_Durability& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Self_AutoAttack(const Self_AutoAttack& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Self_WeaponEnchant(const Self_WeaponEnchant& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Self_Rune(const Self_Rune& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Objective_Progress(const Objective_Progress& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_GoalPart(const GoalPart& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_AchievementPart(const AchievementPart& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Achievement_Criterion(const Achievement_Criterion& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Doing(const Doing& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_HoverRef(const HoverRef& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_WorldState(const WorldState& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_World_Queue(const World_Queue& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Combat_Threat(const Combat_Threat& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Combat_Resources(const Combat_Resources& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Encounter_Timer(const Encounter_Timer& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Instance_Boss(const Instance_Boss& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_Gossip_Option(const Interaction_Gossip_Option& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_RaidBrowserEntry(const Interaction_RaidBrowserEntry& msg,
                                       std::string_view path,
                                       bool in_tooltip,
                                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_Barber(const Interaction_Barber& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_Petition(const Interaction_Petition& msg,
                               std::string_view path,
                               bool in_tooltip,
                               std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_Registrar(const Interaction_Registrar& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_ItemText(const Interaction_ItemText& msg,
                               std::string_view path,
                               bool in_tooltip,
                               std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_Tabard(const Interaction_Tabard& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Interaction_Battlemaster(const Interaction_Battlemaster& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_PetFrame_Totem(const PetFrame_Totem& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Environment_Falling(const Environment_Falling& msg,
                              std::string_view path,
                              bool in_tooltip,
                              std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Battleground_Objective(const Battleground_Objective& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Scoreboard_Row(const Scoreboard_Row& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Bags_Hearthstone(const Bags_Hearthstone& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Character_EquipmentSet(const Character_EquipmentSet& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Talents_Talent(const Talents_Talent& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Talents_Glyph(const Talents_Glyph& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Talents_Spec(const Talents_Spec& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Skills_Skill(const Skills_Skill& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Reputation_Faction(const Reputation_Faction& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Social_Friend(const Social_Friend& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Calendar_Entry(const Calendar_Entry& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_DungeonFinder_Queue(const DungeonFinder_Queue& msg,
                              std::string_view path,
                              bool in_tooltip,
                              std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Pvp_ArenaTeam(const Pvp_ArenaTeam& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_ErrorEvent(const ErrorEvent& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_InfoEvent(const InfoEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_LevelUpEvent(const LevelUpEvent& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_XpGainedEvent(const XpGainedEvent& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_MoneyChangedEvent(const MoneyChangedEvent& msg,
                            std::string_view path,
                            bool in_tooltip,
                            std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_SkillUpEvent(const SkillUpEvent& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_QuestEvent(const QuestEvent& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_DiedEvent(const DiedEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_ResurrectedEvent(const ResurrectedEvent& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_RollEvent(const RollEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_MailEvent(const MailEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_ZoneEvent(const ZoneEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_InstanceEvent(const InstanceEvent& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_CalendarAlarmEvent(const CalendarAlarmEvent& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_DisplacedEvent(const DisplacedEvent& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_DuelEvent(const DuelEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_TitleEvent(const TitleEvent& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_KickedEvent(const KickedEvent& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_HearthstoneBoundEvent(const HearthstoneBoundEvent& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_DbmWarningEvent(const DbmWarningEvent& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_CinematicEvent(const CinematicEvent& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_TalentPointEvent(const TalentPointEvent& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Blank(const Blank& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_Choose(const Command_Choose& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_ClearGoal(const Command_ClearGoal& msg,
                            std::string_view path,
                            bool in_tooltip,
                            std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_Say(const Command_Say& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_Open(const Command_Open& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_Close(const Command_Close& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_AhSearch(const Command_AhSearch& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_Who(const Command_Who& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Relative(const Relative& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.position)) {
    const std::string here = base::StrCat({path, ".position"});
    Walk_Vec3(msg.position, here, in_tooltip, errors);
  }
}

void Walk_Spell(const Spell& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.school)) {
    const std::string here = base::StrCat({path, ".school"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.cost)) {
    const std::string here = base::StrCat({path, ".cost"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    Walk_Spell_Cost(msg.cost.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.cast_time_ms)) {
    const std::string here = base::StrCat({path, ".cast_time_ms"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.range_yd)) {
    const std::string here = base::StrCat({path, ".range_yd"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.tooltip)) {
    const std::string here = base::StrCat({path, ".tooltip"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    Walk_Tooltip(msg.tooltip.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.ability)) {
    const std::string here = base::StrCat({path, ".ability"});
    Walk_AbilityState(msg.ability.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.trainer)) {
    const std::string here = base::StrCat({path, ".trainer"});
    Walk_TrainerSpell(msg.trainer.value(), here, in_tooltip, errors);
  }
}

void Walk_Aura(const Aura& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.tooltip)) {
    const std::string here = base::StrCat({path, ".tooltip"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    Walk_Tooltip(msg.tooltip.value(), here, in_tooltip, errors);
  }
}

void Walk_Self_Indicators(const Self_Indicators& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.durability_warning)) {
    const std::string here = base::StrCat({path, ".durability_warning"});
    for (size_t i = 0; i < msg.durability_warning.size(); ++i) {
      Walk_Self_Indicators_Durability(
          msg.durability_warning[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Item(const Item& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.currency_price)) {
    const std::string here = base::StrCat({path, ".currency_price"});
    for (size_t i = 0; i < msg.currency_price.size(); ++i) {
      Walk_CurrencyAmount(
          msg.currency_price[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.item_level)) {
    const std::string here = base::StrCat({path, ".item_level"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.required_level)) {
    const std::string here = base::StrCat({path, ".required_level"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.binding)) {
    const std::string here = base::StrCat({path, ".binding"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.unique)) {
    const std::string here = base::StrCat({path, ".unique"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.vendor_value)) {
    const std::string here = base::StrCat({path, ".vendor_value"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.tooltip)) {
    const std::string here = base::StrCat({path, ".tooltip"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    Walk_Tooltip(msg.tooltip.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.equipment)) {
    const std::string here = base::StrCat({path, ".equipment"});
    Walk_EquipmentPart(msg.equipment.value(), here, in_tooltip, errors);
  }
}

void Walk_Achievement(const Achievement& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.criteria)) {
    const std::string here = base::StrCat({path, ".criteria"});
    for (size_t i = 0; i < msg.criteria.size(); ++i) {
      Walk_Achievement_Criterion(
          msg.criteria[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Where(const Where& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.position)) {
    const std::string here = base::StrCat({path, ".position"});
    Walk_Vec3(msg.position, here, in_tooltip, errors);
  }
}

void Walk_Ui(const Ui& msg,
             std::string_view path,
             bool in_tooltip,
             std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.hovering)) {
    const std::string here = base::StrCat({path, ".hovering"});
    for (size_t i = 0; i < msg.hovering.size(); ++i) {
      Walk_HoverRef(msg.hovering[i],
                    base::StrCat({here, "[", base::NumberToString(i), "]"}),
                    in_tooltip, errors);
    }
  }
}

void Walk_World(const World& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.world_states)) {
    const std::string here = base::StrCat({path, ".world_states"});
    for (size_t i = 0; i < msg.world_states.size(); ++i) {
      Walk_WorldState(msg.world_states[i],
                      base::StrCat({here, "[", base::NumberToString(i), "]"}),
                      in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.queues)) {
    const std::string here = base::StrCat({path, ".queues"});
    for (size_t i = 0; i < msg.queues.size(); ++i) {
      Walk_World_Queue(msg.queues[i],
                       base::StrCat({here, "[", base::NumberToString(i), "]"}),
                       in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Gossip(const Interaction_Gossip& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.options)) {
    const std::string here = base::StrCat({path, ".options"});
    for (size_t i = 0; i < msg.options.size(); ++i) {
      Walk_Interaction_Gossip_Option(
          msg.options[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Interaction_RaidBrowser(const Interaction_RaidBrowser& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.entries)) {
    const std::string here = base::StrCat({path, ".entries"});
    for (size_t i = 0; i < msg.entries.size(); ++i) {
      Walk_Interaction_RaidBrowserEntry(
          msg.entries[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Environment(const Environment& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.falling)) {
    const std::string here = base::StrCat({path, ".falling"});
    Walk_Environment_Falling(msg.falling.value(), here, in_tooltip, errors);
  }
}

void Walk_Battleground(const Battleground& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.objectives)) {
    const std::string here = base::StrCat({path, ".objectives"});
    for (size_t i = 0; i < msg.objectives.size(); ++i) {
      Walk_Battleground_Objective(
          msg.objectives[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Scoreboard(const Scoreboard& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rows)) {
    const std::string here = base::StrCat({path, ".rows"});
    for (size_t i = 0; i < msg.rows.size(); ++i) {
      Walk_Scoreboard_Row(
          msg.rows[i], base::StrCat({here, "[", base::NumberToString(i), "]"}),
          in_tooltip, errors);
    }
  }
}

void Walk_Map_Zone(const Map_Zone& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.level_range)) {
    const std::string here = base::StrCat({path, ".level_range"});
    Walk_LevelRange(msg.level_range.value(), here, in_tooltip, errors);
  }
}

void Walk_Talents(const Talents& msg,
                  std::string_view path,
                  bool in_tooltip,
                  std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.talents)) {
    const std::string here = base::StrCat({path, ".talents"});
    for (size_t i = 0; i < msg.talents.size(); ++i) {
      Walk_Talents_Talent(
          msg.talents[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.glyphs)) {
    const std::string here = base::StrCat({path, ".glyphs"});
    for (size_t i = 0; i < msg.glyphs.size(); ++i) {
      Walk_Talents_Glyph(
          msg.glyphs[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.specs)) {
    const std::string here = base::StrCat({path, ".specs"});
    for (size_t i = 0; i < msg.specs.size(); ++i) {
      Walk_Talents_Spec(msg.specs[i],
                        base::StrCat({here, "[", base::NumberToString(i), "]"}),
                        in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.pet_talents)) {
    const std::string here = base::StrCat({path, ".pet_talents"});
    for (size_t i = 0; i < msg.pet_talents.size(); ++i) {
      Walk_Talents_Talent(
          msg.pet_talents[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Skills(const Skills& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.skills)) {
    const std::string here = base::StrCat({path, ".skills"});
    for (size_t i = 0; i < msg.skills.size(); ++i) {
      Walk_Skills_Skill(msg.skills[i],
                        base::StrCat({here, "[", base::NumberToString(i), "]"}),
                        in_tooltip, errors);
    }
  }
}

void Walk_Reputation(const Reputation& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.factions)) {
    const std::string here = base::StrCat({path, ".factions"});
    for (size_t i = 0; i < msg.factions.size(); ++i) {
      Walk_Reputation_Faction(
          msg.factions[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Social_Guild(const Social_Guild& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.roster_online)) {
    const std::string here = base::StrCat({path, ".roster_online"});
    for (size_t i = 0; i < msg.roster_online.size(); ++i) {
      Walk_Social_Friend(
          msg.roster_online[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Social_WhoResult(const Social_WhoResult& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.players)) {
    const std::string here = base::StrCat({path, ".players"});
    for (size_t i = 0; i < msg.players.size(); ++i) {
      Walk_Social_Friend(
          msg.players[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Calendar(const Calendar& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.events)) {
    const std::string here = base::StrCat({path, ".events"});
    for (size_t i = 0; i < msg.events.size(); ++i) {
      Walk_Calendar_Entry(
          msg.events[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_DungeonFinder_Dungeon(const DungeonFinder_Dungeon& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.level_range)) {
    const std::string here = base::StrCat({path, ".level_range"});
    Walk_LevelRange(msg.level_range.value(), here, in_tooltip, errors);
  }
}

void Walk_Pvp(const Pvp& msg,
              std::string_view path,
              bool in_tooltip,
              std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.arena_teams)) {
    const std::string here = base::StrCat({path, ".arena_teams"});
    for (size_t i = 0; i < msg.arena_teams.size(); ++i) {
      Walk_Pvp_ArenaTeam(
          msg.arena_teams[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Option(const Option& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.blanks)) {
    const std::string here = base::StrCat({path, ".blanks"});
    for (size_t i = 0; i < msg.blanks.size(); ++i) {
      Walk_Blank(msg.blanks[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_Command_SetGoal(const Command_SetGoal& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  (void)msg;
  (void)path;
  (void)in_tooltip;
  (void)errors;
}

void Walk_Command_Hover(const Command_Hover& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.ref)) {
    const std::string here = base::StrCat({path, ".ref"});
    Walk_HoverRef(msg.ref.value(), here, in_tooltip, errors);
  }
}

void Walk_Cast(const Cast& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.spell)) {
    const std::string here = base::StrCat({path, ".spell"});
    Walk_Spell(msg.spell.value(), here, in_tooltip, errors);
  }
}

void Walk_PetPart(const PetPart& msg,
                  std::string_view path,
                  bool in_tooltip,
                  std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.abilities)) {
    const std::string here = base::StrCat({path, ".abilities"});
    for (size_t i = 0; i < msg.abilities.size(); ++i) {
      Walk_Spell(msg.abilities[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_VehiclePart_Aim(const VehiclePart_Aim& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.impact)) {
    const std::string here = base::StrCat({path, ".impact"});
    Walk_Relative(msg.impact.value(), here, in_tooltip, errors);
  }
}

void Walk_GameObject(const GameObject& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rel)) {
    const std::string here = base::StrCat({path, ".rel"});
    Walk_Relative(msg.rel, here, in_tooltip, errors);
  }
}

void Walk_Area(const Area& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rel)) {
    const std::string here = base::StrCat({path, ".rel"});
    Walk_Relative(msg.rel.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.spell)) {
    const std::string here = base::StrCat({path, ".spell"});
    Walk_Spell(msg.spell.value(), here, in_tooltip, errors);
  }
}

void Walk_Place(const Place& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rel)) {
    const std::string here = base::StrCat({path, ".rel"});
    Walk_Relative(msg.rel, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.level_range)) {
    const std::string here = base::StrCat({path, ".level_range"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    Walk_LevelRange(msg.level_range.value(), here, in_tooltip, errors);
  }
}

void Walk_EquipmentPart_Socket(const EquipmentPart_Socket& msg,
                               std::string_view path,
                               bool in_tooltip,
                               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.gem)) {
    const std::string here = base::StrCat({path, ".gem"});
    Walk_Item(msg.gem.value(), here, in_tooltip, errors);
  }
}

void Walk_QuestPart(const QuestPart& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item_button)) {
    const std::string here = base::StrCat({path, ".item_button"});
    Walk_Spell(msg.item_button.value(), here, in_tooltip, errors);
  }
}

void Walk_ActionBars(const ActionBars& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.buttons)) {
    const std::string here = base::StrCat({path, ".buttons"});
    for (size_t i = 0; i < msg.buttons.size(); ++i) {
      Walk_Spell(msg.buttons[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.stance_bar)) {
    const std::string here = base::StrCat({path, ".stance_bar"});
    for (size_t i = 0; i < msg.stance_bar.size(); ++i) {
      Walk_Spell(msg.stance_bar[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.totem_bar)) {
    const std::string here = base::StrCat({path, ".totem_bar"});
    for (size_t i = 0; i < msg.totem_bar.size(); ++i) {
      Walk_Spell(msg.totem_bar[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Merchant(const Interaction_Merchant& msg,
                               std::string_view path,
                               bool in_tooltip,
                               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.items)) {
    const std::string here = base::StrCat({path, ".items"});
    for (size_t i = 0; i < msg.items.size(); ++i) {
      Walk_Item(msg.items[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.buyback)) {
    const std::string here = base::StrCat({path, ".buyback"});
    for (size_t i = 0; i < msg.buyback.size(); ++i) {
      Walk_Item(msg.buyback[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.my_currency)) {
    const std::string here = base::StrCat({path, ".my_currency"});
    for (size_t i = 0; i < msg.my_currency.size(); ++i) {
      Walk_CurrencyAmount(
          msg.my_currency[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Interaction_QuestDialog(const Interaction_QuestDialog& msg,
                                  std::string_view path,
                                  bool in_tooltip,
                                  std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rewards)) {
    const std::string here = base::StrCat({path, ".rewards"});
    for (size_t i = 0; i < msg.rewards.size(); ++i) {
      Walk_Item(msg.rewards[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.choices)) {
    const std::string here = base::StrCat({path, ".choices"});
    for (size_t i = 0; i < msg.choices.size(); ++i) {
      Walk_Item(msg.choices[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Mail(const Interaction_Mail& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.items)) {
    const std::string here = base::StrCat({path, ".items"});
    for (size_t i = 0; i < msg.items.size(); ++i) {
      Walk_Item(msg.items[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Interaction_AuctionHouse(const Interaction_AuctionHouse& msg,
                                   std::string_view path,
                                   bool in_tooltip,
                                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.my_auctions)) {
    const std::string here = base::StrCat({path, ".my_auctions"});
    for (size_t i = 0; i < msg.my_auctions.size(); ++i) {
      Walk_Item(msg.my_auctions[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.my_bids)) {
    const std::string here = base::StrCat({path, ".my_bids"});
    for (size_t i = 0; i < msg.my_bids.size(); ++i) {
      Walk_Item(msg.my_bids[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.results)) {
    const std::string here = base::StrCat({path, ".results"});
    for (size_t i = 0; i < msg.results.size(); ++i) {
      Walk_Item(msg.results[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Recipe_Reagent(const Interaction_Recipe_Reagent& msg,
                                     std::string_view path,
                                     bool in_tooltip,
                                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_Interaction_Bank(const Interaction_Bank& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.items)) {
    const std::string here = base::StrCat({path, ".items"});
    for (size_t i = 0; i < msg.items.size(); ++i) {
      Walk_Item(msg.items[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Interaction_GuildBank_Tab(const Interaction_GuildBank_Tab& msg,
                                    std::string_view path,
                                    bool in_tooltip,
                                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.items)) {
    const std::string here = base::StrCat({path, ".items"});
    for (size_t i = 0; i < msg.items.size(); ++i) {
      Walk_Item(msg.items[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Trainer(const Interaction_Trainer& msg,
                              std::string_view path,
                              bool in_tooltip,
                              std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.spells)) {
    const std::string here = base::StrCat({path, ".spells"});
    for (size_t i = 0; i < msg.spells.size(); ++i) {
      Walk_Spell(msg.spells[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_TradeLoot_Trade(const TradeLoot_Trade& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.mine)) {
    const std::string here = base::StrCat({path, ".mine"});
    for (size_t i = 0; i < msg.mine.size(); ++i) {
      Walk_Item(msg.mine[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.theirs)) {
    const std::string here = base::StrCat({path, ".theirs"});
    for (size_t i = 0; i < msg.theirs.size(); ++i) {
      Walk_Item(msg.theirs[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.mine_not_traded)) {
    const std::string here = base::StrCat({path, ".mine_not_traded"});
    Walk_Item(msg.mine_not_traded.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.theirs_not_traded)) {
    const std::string here = base::StrCat({path, ".theirs_not_traded"});
    Walk_Item(msg.theirs_not_traded.value(), here, in_tooltip, errors);
  }
}

void Walk_TradeLoot_Roll(const TradeLoot_Roll& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_Surroundings_Obstacle(const Surroundings_Obstacle& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rel)) {
    const std::string here = base::StrCat({path, ".rel"});
    Walk_Relative(msg.rel.value(), here, in_tooltip, errors);
  }
}

void Walk_Bags(const Bags& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.items)) {
    const std::string here = base::StrCat({path, ".items"});
    for (size_t i = 0; i < msg.items.size(); ++i) {
      Walk_Item(msg.items[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.hearthstone)) {
    const std::string here = base::StrCat({path, ".hearthstone"});
    Walk_Bags_Hearthstone(msg.hearthstone.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.keyring)) {
    const std::string here = base::StrCat({path, ".keyring"});
    for (size_t i = 0; i < msg.keyring.size(); ++i) {
      Walk_Item(msg.keyring[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Character(const Character& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.equipped)) {
    const std::string here = base::StrCat({path, ".equipped"});
    for (size_t i = 0; i < msg.equipped.size(); ++i) {
      Walk_Item(msg.equipped[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.stats)) {
    const std::string here = base::StrCat({path, ".stats"});
    for (size_t i = 0; i < msg.stats.size(); ++i) {
      Walk_StatValue(msg.stats[i],
                     base::StrCat({here, "[", base::NumberToString(i), "]"}),
                     in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.equipment_sets)) {
    const std::string here = base::StrCat({path, ".equipment_sets"});
    for (size_t i = 0; i < msg.equipment_sets.size(); ++i) {
      Walk_Character_EquipmentSet(
          msg.equipment_sets[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.currency)) {
    const std::string here = base::StrCat({path, ".currency"});
    for (size_t i = 0; i < msg.currency.size(); ++i) {
      Walk_CurrencyAmount(
          msg.currency[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Companions(const Companions& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.mounts)) {
    const std::string here = base::StrCat({path, ".mounts"});
    for (size_t i = 0; i < msg.mounts.size(); ++i) {
      Walk_Spell(msg.mounts[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.pets)) {
    const std::string here = base::StrCat({path, ".pets"});
    for (size_t i = 0; i < msg.pets.size(); ++i) {
      Walk_Spell(msg.pets[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_Spellbook(const Spellbook& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.spells)) {
    const std::string here = base::StrCat({path, ".spells"});
    for (size_t i = 0; i < msg.spells.size(); ++i) {
      Walk_Spell(msg.spells[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.pet_spells)) {
    const std::string here = base::StrCat({path, ".pet_spells"});
    for (size_t i = 0; i < msg.pet_spells.size(); ++i) {
      Walk_Spell(msg.pet_spells[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_Social(const Social& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.friends)) {
    const std::string here = base::StrCat({path, ".friends"});
    for (size_t i = 0; i < msg.friends.size(); ++i) {
      Walk_Social_Friend(
          msg.friends[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.guild)) {
    const std::string here = base::StrCat({path, ".guild"});
    Walk_Social_Guild(msg.guild.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.who)) {
    const std::string here = base::StrCat({path, ".who"});
    Walk_Social_WhoResult(msg.who.value(), here, in_tooltip, errors);
  }
}

void Walk_Achievements(const Achievements& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.recent)) {
    const std::string here = base::StrCat({path, ".recent"});
    for (size_t i = 0; i < msg.recent.size(); ++i) {
      Walk_Achievement(msg.recent[i],
                       base::StrCat({here, "[", base::NumberToString(i), "]"}),
                       in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.in_category)) {
    const std::string here = base::StrCat({path, ".in_category"});
    for (size_t i = 0; i < msg.in_category.size(); ++i) {
      Walk_Achievement(msg.in_category[i],
                       base::StrCat({here, "[", base::NumberToString(i), "]"}),
                       in_tooltip, errors);
    }
  }
}

void Walk_DungeonFinder(const DungeonFinder& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.random_rewards)) {
    const std::string here = base::StrCat({path, ".random_rewards"});
    for (size_t i = 0; i < msg.random_rewards.size(); ++i) {
      Walk_Item(msg.random_rewards[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.dungeons)) {
    const std::string here = base::StrCat({path, ".dungeons"});
    for (size_t i = 0; i < msg.dungeons.size(); ++i) {
      Walk_DungeonFinder_Dungeon(
          msg.dungeons[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.queue)) {
    const std::string here = base::StrCat({path, ".queue"});
    Walk_DungeonFinder_Queue(msg.queue.value(), here, in_tooltip, errors);
  }
}

void Walk_PromptEvent(const PromptEvent& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_ItemReceivedEvent(const ItemReceivedEvent& msg,
                            std::string_view path,
                            bool in_tooltip,
                            std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_LootRollEvent(const LootRollEvent& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_LootWonEvent(const LootWonEvent& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_LootRollChoiceEvent(const LootRollChoiceEvent& msg,
                              std::string_view path,
                              bool in_tooltip,
                              std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_AuctionEvent(const AuctionEvent& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
}

void Walk_MinimapPingEvent(const MinimapPingEvent& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.where)) {
    const std::string here = base::StrCat({path, ".where"});
    Walk_Relative(msg.where.value(), here, in_tooltip, errors);
  }
}

void Walk_AchievementEvent(const AchievementEvent& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.achievement)) {
    const std::string here = base::StrCat({path, ".achievement"});
    Walk_Achievement(msg.achievement.value(), here, in_tooltip, errors);
  }
}

void Walk_BattlegroundEvent(const BattlegroundEvent& msg,
                            std::string_view path,
                            bool in_tooltip,
                            std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rewards)) {
    const std::string here = base::StrCat({path, ".rewards"});
    for (size_t i = 0; i < msg.rewards.size(); ++i) {
      Walk_Item(msg.rewards[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_LfgEvent(const LfgEvent& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rewards)) {
    const std::string here = base::StrCat({path, ".rewards"});
    for (size_t i = 0; i < msg.rewards.size(); ++i) {
      Walk_Item(msg.rewards[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Command(const Command& msg,
                  std::string_view path,
                  bool in_tooltip,
                  std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.choose)) {
    const std::string here = base::StrCat({path, ".choose"});
    Walk_Command_Choose(msg.choose.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.set_goal)) {
    const std::string here = base::StrCat({path, ".set_goal"});
    Walk_Command_SetGoal(msg.set_goal.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.clear_goal)) {
    const std::string here = base::StrCat({path, ".clear_goal"});
    Walk_Command_ClearGoal(msg.clear_goal.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.say)) {
    const std::string here = base::StrCat({path, ".say"});
    Walk_Command_Say(msg.say.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.open)) {
    const std::string here = base::StrCat({path, ".open"});
    Walk_Command_Open(msg.open.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.close)) {
    const std::string here = base::StrCat({path, ".close"});
    Walk_Command_Close(msg.close.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.hover)) {
    const std::string here = base::StrCat({path, ".hover"});
    Walk_Command_Hover(msg.hover.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.ah_search)) {
    const std::string here = base::StrCat({path, ".ah_search"});
    Walk_Command_AhSearch(msg.ah_search.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.who)) {
    const std::string here = base::StrCat({path, ".who"});
    Walk_Command_Who(msg.who.value(), here, in_tooltip, errors);
  }
}

void Walk_Activity(const Activity& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.cast)) {
    const std::string here = base::StrCat({path, ".cast"});
    Walk_Cast(msg.cast.value(), here, in_tooltip, errors);
  }
}

void Walk_VehiclePart(const VehiclePart& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.seats)) {
    const std::string here = base::StrCat({path, ".seats"});
    for (size_t i = 0; i < msg.seats.size(); ++i) {
      Walk_VehiclePart_Seat(
          msg.seats[i], base::StrCat({here, "[", base::NumberToString(i), "]"}),
          in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.abilities)) {
    const std::string here = base::StrCat({path, ".abilities"});
    for (size_t i = 0; i < msg.abilities.size(); ++i) {
      Walk_Spell(msg.abilities[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.aim)) {
    const std::string here = base::StrCat({path, ".aim"});
    Walk_VehiclePart_Aim(msg.aim.value(), here, in_tooltip, errors);
  }
}

void Walk_EquipmentPart(const EquipmentPart& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.stats)) {
    const std::string here = base::StrCat({path, ".stats"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    for (size_t i = 0; i < msg.stats.size(); ++i) {
      Walk_StatValue(msg.stats[i],
                     base::StrCat({here, "[", base::NumberToString(i), "]"}),
                     in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.sockets)) {
    const std::string here = base::StrCat({path, ".sockets"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    for (size_t i = 0; i < msg.sockets.size(); ++i) {
      Walk_EquipmentPart_Socket(
          msg.sockets[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.socket_bonus)) {
    const std::string here = base::StrCat({path, ".socket_bonus"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.enchant)) {
    const std::string here = base::StrCat({path, ".enchant"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.type)) {
    const std::string here = base::StrCat({path, ".type"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.gearscore)) {
    const std::string here = base::StrCat({path, ".gearscore"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.gearscore_delta)) {
    const std::string here = base::StrCat({path, ".gearscore_delta"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.upgrade)) {
    const std::string here = base::StrCat({path, ".upgrade"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
}

void Walk_Objective(const Objective& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.progress)) {
    const std::string here = base::StrCat({path, ".progress"});
    Walk_Objective_Progress(msg.progress.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.where)) {
    const std::string here = base::StrCat({path, ".where"});
    Walk_Place(msg.where.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest)) {
    const std::string here = base::StrCat({path, ".quest"});
    Walk_QuestPart(msg.quest.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.goal)) {
    const std::string here = base::StrCat({path, ".goal"});
    Walk_GoalPart(msg.goal.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.achievement)) {
    const std::string here = base::StrCat({path, ".achievement"});
    Walk_AchievementPart(msg.achievement.value(), here, in_tooltip, errors);
  }
}

void Walk_Instance(const Instance& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.bosses)) {
    const std::string here = base::StrCat({path, ".bosses"});
    for (size_t i = 0; i < msg.bosses.size(); ++i) {
      Walk_Instance_Boss(
          msg.bosses[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.entrance)) {
    const std::string here = base::StrCat({path, ".entrance"});
    Walk_Place(msg.entrance.value(), here, in_tooltip, errors);
  }
}

void Walk_Death(const Death& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.corpse)) {
    const std::string here = base::StrCat({path, ".corpse"});
    Walk_Relative(msg.corpse.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.spirit_healer)) {
    const std::string here = base::StrCat({path, ".spirit_healer"});
    Walk_Place(msg.spirit_healer.value(), here, in_tooltip, errors);
  }
}

void Walk_Interaction_FlightMap_Destination(
    const Interaction_FlightMap_Destination& msg,
    std::string_view path,
    bool in_tooltip,
    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.place)) {
    const std::string here = base::StrCat({path, ".place"});
    Walk_Place(msg.place.value(), here, in_tooltip, errors);
  }
}

void Walk_Interaction_Recipe(const Interaction_Recipe& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.reagents)) {
    const std::string here = base::StrCat({path, ".reagents"});
    for (size_t i = 0; i < msg.reagents.size(); ++i) {
      Walk_Interaction_Recipe_Reagent(
          msg.reagents[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Interaction_GuildBank(const Interaction_GuildBank& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.tabs)) {
    const std::string here = base::StrCat({path, ".tabs"});
    for (size_t i = 0; i < msg.tabs.size(); ++i) {
      Walk_Interaction_GuildBank_Tab(
          msg.tabs[i], base::StrCat({here, "[", base::NumberToString(i), "]"}),
          in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Socketing(const Interaction_Socketing& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.gems_in_bags)) {
    const std::string here = base::StrCat({path, ".gems_in_bags"});
    for (size_t i = 0; i < msg.gems_in_bags.size(); ++i) {
      Walk_Item(msg.gems_in_bags[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.sockets)) {
    const std::string here = base::StrCat({path, ".sockets"});
    for (size_t i = 0; i < msg.sockets.size(); ++i) {
      Walk_EquipmentPart_Socket(
          msg.sockets[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.pending)) {
    const std::string here = base::StrCat({path, ".pending"});
    for (size_t i = 0; i < msg.pending.size(); ++i) {
      Walk_Item(msg.pending[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_MailInbox(const MailInbox& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.mails)) {
    const std::string here = base::StrCat({path, ".mails"});
    for (size_t i = 0; i < msg.mails.size(); ++i) {
      Walk_Interaction_Mail(
          msg.mails[i], base::StrCat({here, "[", base::NumberToString(i), "]"}),
          in_tooltip, errors);
    }
  }
}

void Walk_TradeLoot(const TradeLoot& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.trade)) {
    const std::string here = base::StrCat({path, ".trade"});
    Walk_TradeLoot_Trade(msg.trade.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.loot)) {
    const std::string here = base::StrCat({path, ".loot"});
    for (size_t i = 0; i < msg.loot.size(); ++i) {
      Walk_Item(msg.loot[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.rolls)) {
    const std::string here = base::StrCat({path, ".rolls"});
    for (size_t i = 0; i < msg.rolls.size(); ++i) {
      Walk_TradeLoot_Roll(
          msg.rolls[i], base::StrCat({here, "[", base::NumberToString(i), "]"}),
          in_tooltip, errors);
    }
  }
}

void Walk_Travel_Transport(const Travel_Transport& msg,
                           std::string_view path,
                           bool in_tooltip,
                           std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.from)) {
    const std::string here = base::StrCat({path, ".from"});
    Walk_Place(msg.from.value(), here, in_tooltip, errors);
  }
}

void Walk_Surroundings(const Surroundings& msg,
                       std::string_view path,
                       bool in_tooltip,
                       std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.obstacles)) {
    const std::string here = base::StrCat({path, ".obstacles"});
    for (size_t i = 0; i < msg.obstacles.size(); ++i) {
      Walk_Surroundings_Obstacle(
          msg.obstacles[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.goal)) {
    const std::string here = base::StrCat({path, ".goal"});
    Walk_Relative(msg.goal.value(), here, in_tooltip, errors);
  }
}

void Walk_Map_Questie(const Map_Questie& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.quest_givers)) {
    const std::string here = base::StrCat({path, ".quest_givers"});
    for (size_t i = 0; i < msg.quest_givers.size(); ++i) {
      Walk_Place(msg.quest_givers[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.spawn_areas)) {
    const std::string here = base::StrCat({path, ".spawn_areas"});
    for (size_t i = 0; i < msg.spawn_areas.size(); ++i) {
      Walk_Place(msg.spawn_areas[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_Map_InstanceMap(const Map_InstanceMap& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.areas)) {
    const std::string here = base::StrCat({path, ".areas"});
    for (size_t i = 0; i < msg.areas.size(); ++i) {
      Walk_Place(msg.areas[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
}

void Walk_ChatLink(const ChatLink& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.spell)) {
    const std::string here = base::StrCat({path, ".spell"});
    Walk_Spell(msg.spell.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest)) {
    const std::string here = base::StrCat({path, ".quest"});
    Walk_QuestPart(msg.quest.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.achievement)) {
    const std::string here = base::StrCat({path, ".achievement"});
    Walk_Achievement(msg.achievement.value(), here, in_tooltip, errors);
  }
}

void Walk_PathFailedEvent(const PathFailedEvent& msg,
                          std::string_view path,
                          bool in_tooltip,
                          std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.to)) {
    const std::string here = base::StrCat({path, ".to"});
    Walk_Place(msg.to.value(), here, in_tooltip, errors);
  }
}

void Walk_Unit(const Unit& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.rel)) {
    const std::string here = base::StrCat({path, ".rel"});
    Walk_Relative(msg.rel, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.activity)) {
    const std::string here = base::StrCat({path, ".activity"});
    Walk_Activity(msg.activity, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.auras)) {
    const std::string here = base::StrCat({path, ".auras"});
    for (size_t i = 0; i < msg.auras.size(); ++i) {
      Walk_Aura(msg.auras[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.pack)) {
    const std::string here = base::StrCat({path, ".pack"});
    Walk_Unit_Pack(msg.pack.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.services)) {
    const std::string here = base::StrCat({path, ".services"});
    if (!(in_tooltip)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
  }
  if (IsFieldPresent(msg.player)) {
    const std::string here = base::StrCat({path, ".player"});
    Walk_PlayerPart(msg.player.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.group)) {
    const std::string here = base::StrCat({path, ".group"});
    Walk_GroupPart(msg.group.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.boss)) {
    const std::string here = base::StrCat({path, ".boss"});
    Walk_BossPart(msg.boss.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.pet)) {
    const std::string here = base::StrCat({path, ".pet"});
    Walk_PetPart(msg.pet.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.vehicle)) {
    const std::string here = base::StrCat({path, ".vehicle"});
    Walk_VehiclePart(msg.vehicle.value(), here, in_tooltip, errors);
  }
}

void Walk_Interaction_FlightMap(const Interaction_FlightMap& msg,
                                std::string_view path,
                                bool in_tooltip,
                                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.destinations)) {
    const std::string here = base::StrCat({path, ".destinations"});
    for (size_t i = 0; i < msg.destinations.size(); ++i) {
      Walk_Interaction_FlightMap_Destination(
          msg.destinations[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Interaction_Tradeskill(const Interaction_Tradeskill& msg,
                                 std::string_view path,
                                 bool in_tooltip,
                                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.recipes)) {
    const std::string here = base::StrCat({path, ".recipes"});
    for (size_t i = 0; i < msg.recipes.size(); ++i) {
      Walk_Interaction_Recipe(
          msg.recipes[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Travel(const Travel& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.known_flight_paths)) {
    const std::string here = base::StrCat({path, ".known_flight_paths"});
    for (size_t i = 0; i < msg.known_flight_paths.size(); ++i) {
      Walk_Place(msg.known_flight_paths[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.nearest_flight_master)) {
    const std::string here = base::StrCat({path, ".nearest_flight_master"});
    Walk_Place(msg.nearest_flight_master.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.mount)) {
    const std::string here = base::StrCat({path, ".mount"});
    Walk_Item(msg.mount.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.transports)) {
    const std::string here = base::StrCat({path, ".transports"});
    for (size_t i = 0; i < msg.transports.size(); ++i) {
      Walk_Travel_Transport(
          msg.transports[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Map(const Map& msg,
              std::string_view path,
              bool in_tooltip,
              std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.zone)) {
    const std::string here = base::StrCat({path, ".zone"});
    Walk_Map_Zone(msg.zone.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.places)) {
    const std::string here = base::StrCat({path, ".places"});
    for (size_t i = 0; i < msg.places.size(); ++i) {
      Walk_Place(msg.places[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.bordering_zones)) {
    const std::string here = base::StrCat({path, ".bordering_zones"});
    for (size_t i = 0; i < msg.bordering_zones.size(); ++i) {
      Walk_Place(msg.bordering_zones[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.objective_locations)) {
    const std::string here = base::StrCat({path, ".objective_locations"});
    for (size_t i = 0; i < msg.objective_locations.size(); ++i) {
      Walk_Place(msg.objective_locations[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.questie)) {
    const std::string here = base::StrCat({path, ".questie"});
    Walk_Map_Questie(msg.questie.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.instance_map)) {
    const std::string here = base::StrCat({path, ".instance_map"});
    Walk_Map_InstanceMap(msg.instance_map.value(), here, in_tooltip, errors);
  }
}

void Walk_QuestLog_Entry(const QuestLog_Entry& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.objective)) {
    const std::string here = base::StrCat({path, ".objective"});
    Walk_Objective(msg.objective.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.rewards)) {
    const std::string here = base::StrCat({path, ".rewards"});
    for (size_t i = 0; i < msg.rewards.size(); ++i) {
      Walk_Item(msg.rewards[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.choices)) {
    const std::string here = base::StrCat({path, ".choices"});
    for (size_t i = 0; i < msg.choices.size(); ++i) {
      Walk_Item(msg.choices[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_ChatEvent(const ChatEvent& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.links)) {
    const std::string here = base::StrCat({path, ".links"});
    for (size_t i = 0; i < msg.links.size(); ++i) {
      Walk_ChatLink(msg.links[i],
                    base::StrCat({here, "[", base::NumberToString(i), "]"}),
                    in_tooltip, errors);
    }
  }
}

void Walk_Self(const Self& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.unit)) {
    const std::string here = base::StrCat({path, ".unit"});
    Walk_Unit(msg.unit, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.control)) {
    const std::string here = base::StrCat({path, ".control"});
    Walk_Self_Control(msg.control, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.casting)) {
    const std::string here = base::StrCat({path, ".casting"});
    Walk_Cast(msg.casting.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.indicators)) {
    const std::string here = base::StrCat({path, ".indicators"});
    Walk_Self_Indicators(msg.indicators, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.auto_attack)) {
    const std::string here = base::StrCat({path, ".auto_attack"});
    Walk_Self_AutoAttack(msg.auto_attack.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.weapon_enchants)) {
    const std::string here = base::StrCat({path, ".weapon_enchants"});
    for (size_t i = 0; i < msg.weapon_enchants.size(); ++i) {
      Walk_Self_WeaponEnchant(
          msg.weapon_enchants[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.runes)) {
    const std::string here = base::StrCat({path, ".runes"});
    for (size_t i = 0; i < msg.runes.size(); ++i) {
      Walk_Self_Rune(msg.runes[i],
                     base::StrCat({here, "[", base::NumberToString(i), "]"}),
                     in_tooltip, errors);
    }
  }
}

void Walk_Nearby(const Nearby& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.units)) {
    const std::string here = base::StrCat({path, ".units"});
    for (size_t i = 0; i < msg.units.size(); ++i) {
      Walk_Unit(msg.units[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.objects)) {
    const std::string here = base::StrCat({path, ".objects"});
    for (size_t i = 0; i < msg.objects.size(); ++i) {
      Walk_GameObject(msg.objects[i],
                      base::StrCat({here, "[", base::NumberToString(i), "]"}),
                      in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.areas)) {
    const std::string here = base::StrCat({path, ".areas"});
    for (size_t i = 0; i < msg.areas.size(); ++i) {
      Walk_Area(msg.areas[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Tooltipped(const Tooltipped& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.ref)) {
    const std::string here = base::StrCat({path, ".ref"});
    Walk_HoverRef(msg.ref.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.tooltip)) {
    const std::string here = base::StrCat({path, ".tooltip"});
    Walk_Tooltip(msg.tooltip.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.item)) {
    const std::string here = base::StrCat({path, ".item"});
    Walk_Item(msg.item.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.spell)) {
    const std::string here = base::StrCat({path, ".spell"});
    Walk_Spell(msg.spell.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.aura)) {
    const std::string here = base::StrCat({path, ".aura"});
    Walk_Aura(msg.aura.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.unit)) {
    const std::string here = base::StrCat({path, ".unit"});
    Walk_Unit(msg.unit.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.place)) {
    const std::string here = base::StrCat({path, ".place"});
    Walk_Place(msg.place.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.achievement)) {
    const std::string here = base::StrCat({path, ".achievement"});
    Walk_Achievement(msg.achievement.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.game_object)) {
    const std::string here = base::StrCat({path, ".game_object"});
    Walk_GameObject(msg.game_object.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.compare)) {
    const std::string here = base::StrCat({path, ".compare"});
    for (size_t i = 0; i < msg.compare.size(); ++i) {
      Walk_Item(msg.compare[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Minimap(const Minimap& msg,
                  std::string_view path,
                  bool in_tooltip,
                  std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.places)) {
    const std::string here = base::StrCat({path, ".places"});
    for (size_t i = 0; i < msg.places.size(); ++i) {
      Walk_Place(msg.places[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.objective_areas)) {
    const std::string here = base::StrCat({path, ".objective_areas"});
    for (size_t i = 0; i < msg.objective_areas.size(); ++i) {
      Walk_Place(msg.objective_areas[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.tracked)) {
    const std::string here = base::StrCat({path, ".tracked"});
    for (size_t i = 0; i < msg.tracked.size(); ++i) {
      Walk_Unit(msg.tracked[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.tracked_objects)) {
    const std::string here = base::StrCat({path, ".tracked_objects"});
    for (size_t i = 0; i < msg.tracked_objects.size(); ++i) {
      Walk_GameObject(msg.tracked_objects[i],
                      base::StrCat({here, "[", base::NumberToString(i), "]"}),
                      in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.party)) {
    const std::string here = base::StrCat({path, ".party"});
    for (size_t i = 0; i < msg.party.size(); ++i) {
      Walk_Unit(msg.party[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.corpse)) {
    const std::string here = base::StrCat({path, ".corpse"});
    Walk_Relative(msg.corpse.value(), here, in_tooltip, errors);
  }
}

void Walk_Target(const Target& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.target)) {
    const std::string here = base::StrCat({path, ".target"});
    Walk_Unit(msg.target.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.focus)) {
    const std::string here = base::StrCat({path, ".focus"});
    Walk_Unit(msg.focus.value(), here, in_tooltip, errors);
  }
}

void Walk_Combat(const Combat& msg,
                 std::string_view path,
                 bool in_tooltip,
                 std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.attackers)) {
    const std::string here = base::StrCat({path, ".attackers"});
    for (size_t i = 0; i < msg.attackers.size(); ++i) {
      Walk_Unit(msg.attackers[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.threat)) {
    const std::string here = base::StrCat({path, ".threat"});
    for (size_t i = 0; i < msg.threat.size(); ++i) {
      Walk_Combat_Threat(
          msg.threat[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
  if (IsFieldPresent(msg.resources)) {
    const std::string here = base::StrCat({path, ".resources"});
    Walk_Combat_Resources(msg.resources.value(), here, in_tooltip, errors);
  }
}

void Walk_Party(const Party& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.members)) {
    const std::string here = base::StrCat({path, ".members"});
    for (size_t i = 0; i < msg.members.size(); ++i) {
      Walk_Unit(msg.members[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Raid(const Raid& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.members)) {
    const std::string here = base::StrCat({path, ".members"});
    for (size_t i = 0; i < msg.members.size(); ++i) {
      Walk_Unit(msg.members[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Encounter(const Encounter& msg,
                    std::string_view path,
                    bool in_tooltip,
                    std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.boss)) {
    const std::string here = base::StrCat({path, ".boss"});
    Walk_Unit(msg.boss.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.dbm)) {
    const std::string here = base::StrCat({path, ".dbm"});
    for (size_t i = 0; i < msg.dbm.size(); ++i) {
      Walk_Encounter_Timer(
          msg.dbm[i], base::StrCat({here, "[", base::NumberToString(i), "]"}),
          in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Inspect(const Interaction_Inspect& msg,
                              std::string_view path,
                              bool in_tooltip,
                              std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.player)) {
    const std::string here = base::StrCat({path, ".player"});
    Walk_Unit(msg.player.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.equipped)) {
    const std::string here = base::StrCat({path, ".equipped"});
    for (size_t i = 0; i < msg.equipped.size(); ++i) {
      Walk_Item(msg.equipped[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_Interaction_Stable(const Interaction_Stable& msg,
                             std::string_view path,
                             bool in_tooltip,
                             std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.stabled)) {
    const std::string here = base::StrCat({path, ".stabled"});
    for (size_t i = 0; i < msg.stabled.size(); ++i) {
      Walk_Unit(msg.stabled[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_PetFrame(const PetFrame& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.pet)) {
    const std::string here = base::StrCat({path, ".pet"});
    Walk_Unit(msg.pet.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.totems)) {
    const std::string here = base::StrCat({path, ".totems"});
    for (size_t i = 0; i < msg.totems.size(); ++i) {
      Walk_PetFrame_Totem(
          msg.totems[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Arena(const Arena& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.enemies)) {
    const std::string here = base::StrCat({path, ".enemies"});
    for (size_t i = 0; i < msg.enemies.size(); ++i) {
      Walk_Unit(msg.enemies[i],
                base::StrCat({here, "[", base::NumberToString(i), "]"}),
                in_tooltip, errors);
    }
  }
}

void Walk_QuestLog(const QuestLog& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.quests)) {
    const std::string here = base::StrCat({path, ".quests"});
    for (size_t i = 0; i < msg.quests.size(); ++i) {
      Walk_QuestLog_Entry(
          msg.quests[i],
          base::StrCat({here, "[", base::NumberToString(i), "]"}), in_tooltip,
          errors);
    }
  }
}

void Walk_Event(const Event& msg,
                std::string_view path,
                bool in_tooltip,
                std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.chat)) {
    const std::string here = base::StrCat({path, ".chat"});
    Walk_ChatEvent(msg.chat.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.error)) {
    const std::string here = base::StrCat({path, ".error"});
    Walk_ErrorEvent(msg.error.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.info)) {
    const std::string here = base::StrCat({path, ".info"});
    Walk_InfoEvent(msg.info.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.prompt)) {
    const std::string here = base::StrCat({path, ".prompt"});
    Walk_PromptEvent(msg.prompt.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.level_up)) {
    const std::string here = base::StrCat({path, ".level_up"});
    Walk_LevelUpEvent(msg.level_up.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.xp_gained)) {
    const std::string here = base::StrCat({path, ".xp_gained"});
    Walk_XpGainedEvent(msg.xp_gained.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.money_changed)) {
    const std::string here = base::StrCat({path, ".money_changed"});
    Walk_MoneyChangedEvent(msg.money_changed.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.item_received)) {
    const std::string here = base::StrCat({path, ".item_received"});
    Walk_ItemReceivedEvent(msg.item_received.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.skill_up)) {
    const std::string here = base::StrCat({path, ".skill_up"});
    Walk_SkillUpEvent(msg.skill_up.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest_complete)) {
    const std::string here = base::StrCat({path, ".quest_complete"});
    Walk_QuestEvent(msg.quest_complete.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest_failed)) {
    const std::string here = base::StrCat({path, ".quest_failed"});
    Walk_QuestEvent(msg.quest_failed.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest_accepted)) {
    const std::string here = base::StrCat({path, ".quest_accepted"});
    Walk_QuestEvent(msg.quest_accepted.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.died)) {
    const std::string here = base::StrCat({path, ".died"});
    Walk_DiedEvent(msg.died.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.resurrected)) {
    const std::string here = base::StrCat({path, ".resurrected"});
    Walk_ResurrectedEvent(msg.resurrected.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.loot_roll)) {
    const std::string here = base::StrCat({path, ".loot_roll"});
    Walk_LootRollEvent(msg.loot_roll.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.loot_won)) {
    const std::string here = base::StrCat({path, ".loot_won"});
    Walk_LootWonEvent(msg.loot_won.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.roll)) {
    const std::string here = base::StrCat({path, ".roll"});
    Walk_RollEvent(msg.roll.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.mail_received)) {
    const std::string here = base::StrCat({path, ".mail_received"});
    Walk_MailEvent(msg.mail_received.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.auction)) {
    const std::string here = base::StrCat({path, ".auction"});
    Walk_AuctionEvent(msg.auction.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.zone_changed)) {
    const std::string here = base::StrCat({path, ".zone_changed"});
    Walk_ZoneEvent(msg.zone_changed.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.discovered_area)) {
    const std::string here = base::StrCat({path, ".discovered_area"});
    Walk_ZoneEvent(msg.discovered_area.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.instance)) {
    const std::string here = base::StrCat({path, ".instance"});
    Walk_InstanceEvent(msg.instance.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.path_failed)) {
    const std::string here = base::StrCat({path, ".path_failed"});
    Walk_PathFailedEvent(msg.path_failed.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.duel)) {
    const std::string here = base::StrCat({path, ".duel"});
    Walk_DuelEvent(msg.duel.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.achievement_earned)) {
    const std::string here = base::StrCat({path, ".achievement_earned"});
    Walk_AchievementEvent(msg.achievement_earned.value(), here, in_tooltip,
                          errors);
  }
  if (IsFieldPresent(msg.title_earned)) {
    const std::string here = base::StrCat({path, ".title_earned"});
    Walk_TitleEvent(msg.title_earned.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.kicked)) {
    const std::string here = base::StrCat({path, ".kicked"});
    Walk_KickedEvent(msg.kicked.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.hearthstone_bound)) {
    const std::string here = base::StrCat({path, ".hearthstone_bound"});
    Walk_HearthstoneBoundEvent(msg.hearthstone_bound.value(), here, in_tooltip,
                               errors);
  }
  if (IsFieldPresent(msg.battleground)) {
    const std::string here = base::StrCat({path, ".battleground"});
    Walk_BattlegroundEvent(msg.battleground.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.dbm_warning)) {
    const std::string here = base::StrCat({path, ".dbm_warning"});
    Walk_DbmWarningEvent(msg.dbm_warning.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.cinematic)) {
    const std::string here = base::StrCat({path, ".cinematic"});
    Walk_CinematicEvent(msg.cinematic.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.talent_point_gained)) {
    const std::string here = base::StrCat({path, ".talent_point_gained"});
    Walk_TalentPointEvent(msg.talent_point_gained.value(), here, in_tooltip,
                          errors);
  }
  if (IsFieldPresent(msg.loot_roll_choice)) {
    const std::string here = base::StrCat({path, ".loot_roll_choice"});
    Walk_LootRollChoiceEvent(msg.loot_roll_choice.value(), here, in_tooltip,
                             errors);
  }
  if (IsFieldPresent(msg.lfg)) {
    const std::string here = base::StrCat({path, ".lfg"});
    Walk_LfgEvent(msg.lfg.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.minimap_ping)) {
    const std::string here = base::StrCat({path, ".minimap_ping"});
    Walk_MinimapPingEvent(msg.minimap_ping.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.calendar_alarm)) {
    const std::string here = base::StrCat({path, ".calendar_alarm"});
    Walk_CalendarAlarmEvent(msg.calendar_alarm.value(), here, in_tooltip,
                            errors);
  }
  if (IsFieldPresent(msg.displaced)) {
    const std::string here = base::StrCat({path, ".displaced"});
    Walk_DisplacedEvent(msg.displaced.value(), here, in_tooltip, errors);
  }
}

void Walk_Base(const Base& msg,
               std::string_view path,
               bool in_tooltip,
               std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.self)) {
    const std::string here = base::StrCat({path, ".self"});
    Walk_Self(msg.self, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.where)) {
    const std::string here = base::StrCat({path, ".where"});
    Walk_Where(msg.where, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.doing)) {
    const std::string here = base::StrCat({path, ".doing"});
    Walk_Doing(msg.doing.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.nearby)) {
    const std::string here = base::StrCat({path, ".nearby"});
    Walk_Nearby(msg.nearby, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.ui)) {
    const std::string here = base::StrCat({path, ".ui"});
    Walk_Ui(msg.ui, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.tooltips)) {
    const std::string here = base::StrCat({path, ".tooltips"});
    if (!(true)) {
      errors->push_back(
          base::StrCat({here, ": hover-only field outside base.tooltips"}));
    }
    for (size_t i = 0; i < msg.tooltips.size(); ++i) {
      Walk_Tooltipped(msg.tooltips[i],
                      base::StrCat({here, "[", base::NumberToString(i), "]"}),
                      true, errors);
    }
  }
}

void Walk_Interaction(const Interaction& msg,
                      std::string_view path,
                      bool in_tooltip,
                      std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.merchant)) {
    const std::string here = base::StrCat({path, ".merchant"});
    Walk_Interaction_Merchant(msg.merchant.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.trainer)) {
    const std::string here = base::StrCat({path, ".trainer"});
    Walk_Interaction_Trainer(msg.trainer.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest)) {
    const std::string here = base::StrCat({path, ".quest"});
    Walk_Interaction_QuestDialog(msg.quest.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.gossip)) {
    const std::string here = base::StrCat({path, ".gossip"});
    Walk_Interaction_Gossip(msg.gossip.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.flight_map)) {
    const std::string here = base::StrCat({path, ".flight_map"});
    Walk_Interaction_FlightMap(msg.flight_map.value(), here, in_tooltip,
                               errors);
  }
  if (IsFieldPresent(msg.bank)) {
    const std::string here = base::StrCat({path, ".bank"});
    Walk_Interaction_Bank(msg.bank.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.mail)) {
    const std::string here = base::StrCat({path, ".mail"});
    Walk_MailInbox(msg.mail.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.auction_house)) {
    const std::string here = base::StrCat({path, ".auction_house"});
    Walk_Interaction_AuctionHouse(msg.auction_house.value(), here, in_tooltip,
                                  errors);
  }
  if (IsFieldPresent(msg.tradeskill)) {
    const std::string here = base::StrCat({path, ".tradeskill"});
    Walk_Interaction_Tradeskill(msg.tradeskill.value(), here, in_tooltip,
                                errors);
  }
  if (IsFieldPresent(msg.inspect)) {
    const std::string here = base::StrCat({path, ".inspect"});
    Walk_Interaction_Inspect(msg.inspect.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.item_text)) {
    const std::string here = base::StrCat({path, ".item_text"});
    Walk_Interaction_ItemText(msg.item_text.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.raid_browser)) {
    const std::string here = base::StrCat({path, ".raid_browser"});
    Walk_Interaction_RaidBrowser(msg.raid_browser.value(), here, in_tooltip,
                                 errors);
  }
  if (IsFieldPresent(msg.guild_bank)) {
    const std::string here = base::StrCat({path, ".guild_bank"});
    Walk_Interaction_GuildBank(msg.guild_bank.value(), here, in_tooltip,
                               errors);
  }
  if (IsFieldPresent(msg.socketing)) {
    const std::string here = base::StrCat({path, ".socketing"});
    Walk_Interaction_Socketing(msg.socketing.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.barber)) {
    const std::string here = base::StrCat({path, ".barber"});
    Walk_Interaction_Barber(msg.barber.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.stable)) {
    const std::string here = base::StrCat({path, ".stable"});
    Walk_Interaction_Stable(msg.stable.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.petition)) {
    const std::string here = base::StrCat({path, ".petition"});
    Walk_Interaction_Petition(msg.petition.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.guild_registrar)) {
    const std::string here = base::StrCat({path, ".guild_registrar"});
    Walk_Interaction_Registrar(msg.guild_registrar.value(), here, in_tooltip,
                               errors);
  }
  if (IsFieldPresent(msg.tabard)) {
    const std::string here = base::StrCat({path, ".tabard"});
    Walk_Interaction_Tabard(msg.tabard.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.battlemaster)) {
    const std::string here = base::StrCat({path, ".battlemaster"});
    Walk_Interaction_Battlemaster(msg.battlemaster.value(), here, in_tooltip,
                                  errors);
  }
}

void Walk_ImmediateEvent(const ImmediateEvent& msg,
                         std::string_view path,
                         bool in_tooltip,
                         std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.event)) {
    const std::string here = base::StrCat({path, ".event"});
    Walk_Event(msg.event, here, in_tooltip, errors);
  }
}

void Walk_Extensions(const Extensions& msg,
                     std::string_view path,
                     bool in_tooltip,
                     std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.minimap)) {
    const std::string here = base::StrCat({path, ".minimap"});
    Walk_Minimap(msg.minimap.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest_tracker)) {
    const std::string here = base::StrCat({path, ".quest_tracker"});
    for (size_t i = 0; i < msg.quest_tracker.size(); ++i) {
      Walk_Objective(msg.quest_tracker[i],
                     base::StrCat({here, "[", base::NumberToString(i), "]"}),
                     in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.action_bars)) {
    const std::string here = base::StrCat({path, ".action_bars"});
    Walk_ActionBars(msg.action_bars.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.world)) {
    const std::string here = base::StrCat({path, ".world"});
    Walk_World(msg.world.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.target)) {
    const std::string here = base::StrCat({path, ".target"});
    Walk_Target(msg.target.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.combat)) {
    const std::string here = base::StrCat({path, ".combat"});
    Walk_Combat(msg.combat.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.party)) {
    const std::string here = base::StrCat({path, ".party"});
    Walk_Party(msg.party.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.raid)) {
    const std::string here = base::StrCat({path, ".raid"});
    Walk_Raid(msg.raid.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.encounter)) {
    const std::string here = base::StrCat({path, ".encounter"});
    Walk_Encounter(msg.encounter.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.instance)) {
    const std::string here = base::StrCat({path, ".instance"});
    Walk_Instance(msg.instance.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.death)) {
    const std::string here = base::StrCat({path, ".death"});
    Walk_Death(msg.death.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.vehicle)) {
    const std::string here = base::StrCat({path, ".vehicle"});
    Walk_Unit(msg.vehicle.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.interaction)) {
    const std::string here = base::StrCat({path, ".interaction"});
    Walk_Interaction(msg.interaction.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.trade_loot)) {
    const std::string here = base::StrCat({path, ".trade_loot"});
    Walk_TradeLoot(msg.trade_loot.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.travel)) {
    const std::string here = base::StrCat({path, ".travel"});
    Walk_Travel(msg.travel.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.pet)) {
    const std::string here = base::StrCat({path, ".pet"});
    Walk_PetFrame(msg.pet.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.environment)) {
    const std::string here = base::StrCat({path, ".environment"});
    Walk_Environment(msg.environment.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.battleground)) {
    const std::string here = base::StrCat({path, ".battleground"});
    Walk_Battleground(msg.battleground.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.surroundings)) {
    const std::string here = base::StrCat({path, ".surroundings"});
    Walk_Surroundings(msg.surroundings.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.possessed)) {
    const std::string here = base::StrCat({path, ".possessed"});
    Walk_Unit(msg.possessed.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.arena)) {
    const std::string here = base::StrCat({path, ".arena"});
    Walk_Arena(msg.arena.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.bags)) {
    const std::string here = base::StrCat({path, ".bags"});
    Walk_Bags(msg.bags.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.map)) {
    const std::string here = base::StrCat({path, ".map"});
    Walk_Map(msg.map.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.character)) {
    const std::string here = base::StrCat({path, ".character"});
    Walk_Character(msg.character.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.quest_log)) {
    const std::string here = base::StrCat({path, ".quest_log"});
    Walk_QuestLog(msg.quest_log.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.spellbook)) {
    const std::string here = base::StrCat({path, ".spellbook"});
    Walk_Spellbook(msg.spellbook.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.talents)) {
    const std::string here = base::StrCat({path, ".talents"});
    Walk_Talents(msg.talents.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.skills)) {
    const std::string here = base::StrCat({path, ".skills"});
    Walk_Skills(msg.skills.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.reputation)) {
    const std::string here = base::StrCat({path, ".reputation"});
    Walk_Reputation(msg.reputation.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.social)) {
    const std::string here = base::StrCat({path, ".social"});
    Walk_Social(msg.social.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.calendar)) {
    const std::string here = base::StrCat({path, ".calendar"});
    Walk_Calendar(msg.calendar.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.achievements)) {
    const std::string here = base::StrCat({path, ".achievements"});
    Walk_Achievements(msg.achievements.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.pvp)) {
    const std::string here = base::StrCat({path, ".pvp"});
    Walk_Pvp(msg.pvp.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.companions)) {
    const std::string here = base::StrCat({path, ".companions"});
    Walk_Companions(msg.companions.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.dungeon_finder)) {
    const std::string here = base::StrCat({path, ".dungeon_finder"});
    Walk_DungeonFinder(msg.dungeon_finder.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.scoreboard)) {
    const std::string here = base::StrCat({path, ".scoreboard"});
    Walk_Scoreboard(msg.scoreboard.value(), here, in_tooltip, errors);
  }
}

void Walk_Snapshot(const Snapshot& msg,
                   std::string_view path,
                   bool in_tooltip,
                   std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.base)) {
    const std::string here = base::StrCat({path, ".base"});
    Walk_Base(msg.base, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.ext)) {
    const std::string here = base::StrCat({path, ".ext"});
    Walk_Extensions(msg.ext, here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.events)) {
    const std::string here = base::StrCat({path, ".events"});
    for (size_t i = 0; i < msg.events.size(); ++i) {
      Walk_Event(msg.events[i],
                 base::StrCat({here, "[", base::NumberToString(i), "]"}),
                 in_tooltip, errors);
    }
  }
  if (IsFieldPresent(msg.options)) {
    const std::string here = base::StrCat({path, ".options"});
    for (size_t i = 0; i < msg.options.size(); ++i) {
      Walk_Option(msg.options[i],
                  base::StrCat({here, "[", base::NumberToString(i), "]"}),
                  in_tooltip, errors);
    }
  }
}

void Walk_ModuleMessage(const ModuleMessage& msg,
                        std::string_view path,
                        bool in_tooltip,
                        std::vector<std::string>* errors) {
  if (IsFieldPresent(msg.snapshot)) {
    const std::string here = base::StrCat({path, ".snapshot"});
    Walk_Snapshot(msg.snapshot.value(), here, in_tooltip, errors);
  }
  if (IsFieldPresent(msg.event)) {
    const std::string here = base::StrCat({path, ".event"});
    Walk_ImmediateEvent(msg.event.value(), here, in_tooltip, errors);
  }
}

}  // namespace

std::vector<std::string> ValidateSnapshotVisibility(const Snapshot& snap) {
  std::vector<std::string> errors;
  Walk_Snapshot(snap, "$", false, &errors);
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
  std::string out;
  (void)glz::write<WireBeveOpts{}>(msg, out);
  return out;
}

bool ReadBeve(std::string_view bytes, ModuleMessage* out) {
  *out = ModuleMessage{};
  return !glz::read<WireBeveOpts{}>(*out, bytes);
}

std::string WriteBeve(const Command& cmd) {
  std::string out;
  (void)glz::write<WireBeveOpts{}>(cmd, out);
  return out;
}

bool ReadBeve(std::string_view bytes, Command* out) {
  *out = Command{};
  return !glz::read<WireBeveOpts{}>(*out, bytes);
}

std::string WriteBeve(const Snapshot& snap) {
  std::string out;
  (void)glz::write<WireBeveOpts{}>(snap, out);
  return out;
}

bool ReadBeve(std::string_view bytes, Snapshot* out) {
  *out = Snapshot{};
  return !glz::read<WireBeveOpts{}>(*out, bytes);
}

std::string WriteJson(const Snapshot& snap, bool pretty) {
  std::string out;
  // An enum value with no name (a newer peer's, or memory gone bad) makes the
  // writer fail half way, leaving invalid JSON: return nothing instead.
  const auto ec = pretty ? glz::write<PrettyJsonWriteOpts{}>(snap, out)
                         : glz::write<CompactJsonWriteOpts{}>(snap, out);
  if (ec) {
    return "";
  }
  return out;
}

bool ReadJson(std::string_view json, Snapshot* out, std::string* error) {
  *out = Snapshot{};
  const auto ec = glz::read<StrictJsonReadOpts{}>(*out, json);
  if (ec) {
    if (error != nullptr) {
      *error = glz::format_error(ec, json);
    }
    return false;
  }
  return true;
}

std::string WriteJson(const ModuleMessage& msg, bool pretty) {
  std::string out;
  // An enum value with no name (a newer peer's, or memory gone bad) makes the
  // writer fail half way, leaving invalid JSON: return nothing instead.
  const auto ec = pretty ? glz::write<PrettyJsonWriteOpts{}>(msg, out)
                         : glz::write<CompactJsonWriteOpts{}>(msg, out);
  if (ec) {
    return "";
  }
  return out;
}

bool ReadJson(std::string_view json, ModuleMessage* out, std::string* error) {
  *out = ModuleMessage{};
  const auto ec = glz::read<StrictJsonReadOpts{}>(*out, json);
  if (ec) {
    if (error != nullptr) {
      *error = glz::format_error(ec, json);
    }
    return false;
  }
  return true;
}

std::string WriteJson(const Command& cmd, bool pretty) {
  std::string out;
  // An enum value with no name (a newer peer's, or memory gone bad) makes the
  // writer fail half way, leaving invalid JSON: return nothing instead.
  const auto ec = pretty ? glz::write<PrettyJsonWriteOpts{}>(cmd, out)
                         : glz::write<CompactJsonWriteOpts{}>(cmd, out);
  if (ec) {
    return "";
  }
  return out;
}

bool ReadJson(std::string_view json, Command* out, std::string* error) {
  *out = Command{};
  const auto ec = glz::read<StrictJsonReadOpts{}>(*out, json);
  if (ec) {
    if (error != nullptr) {
      *error = glz::format_error(ec, json);
    }
    return false;
  }
  return true;
}

std::string_view EnumName(Direction value) {
  switch (value) {
    case Direction::DIRECTION_UNSPECIFIED:
      return "DIRECTION_UNSPECIFIED";
    case Direction::HERE:
      return "HERE";
    case Direction::N:
      return "N";
    case Direction::NE:
      return "NE";
    case Direction::E:
      return "E";
    case Direction::SE:
      return "SE";
    case Direction::S:
      return "S";
    case Direction::SW:
      return "SW";
    case Direction::W:
      return "W";
    case Direction::NW:
      return "NW";
  }
  return "";
}

std::string_view EnumName(DispelType value) {
  switch (value) {
    case DispelType::DISPEL_TYPE_UNSPECIFIED:
      return "DISPEL_TYPE_UNSPECIFIED";
    case DispelType::DISPEL_NONE:
      return "DISPEL_NONE";
    case DispelType::MAGIC:
      return "MAGIC";
    case DispelType::CURSE:
      return "CURSE";
    case DispelType::POISON:
      return "POISON";
    case DispelType::DISEASE:
      return "DISEASE";
  }
  return "";
}

std::string_view EnumName(RaidMarker value) {
  switch (value) {
    case RaidMarker::RAID_MARKER_UNSPECIFIED:
      return "RAID_MARKER_UNSPECIFIED";
    case RaidMarker::STAR:
      return "STAR";
    case RaidMarker::CIRCLE:
      return "CIRCLE";
    case RaidMarker::DIAMOND:
      return "DIAMOND";
    case RaidMarker::TRIANGLE:
      return "TRIANGLE";
    case RaidMarker::MOON:
      return "MOON";
    case RaidMarker::SQUARE:
      return "SQUARE";
    case RaidMarker::CROSS:
      return "CROSS";
    case RaidMarker::SKULL:
      return "SKULL";
  }
  return "";
}

std::string_view EnumName(Role value) {
  switch (value) {
    case Role::ROLE_UNSPECIFIED:
      return "ROLE_UNSPECIFIED";
    case Role::TANK:
      return "TANK";
    case Role::HEALER:
      return "HEALER";
    case Role::DPS:
      return "DPS";
  }
  return "";
}

std::string_view EnumName(Stat value) {
  switch (value) {
    case Stat::STAT_UNSPECIFIED:
      return "STAT_UNSPECIFIED";
    case Stat::STRENGTH:
      return "STRENGTH";
    case Stat::AGILITY:
      return "AGILITY";
    case Stat::STAMINA:
      return "STAMINA";
    case Stat::INTELLECT:
      return "INTELLECT";
    case Stat::SPIRIT:
      return "SPIRIT";
    case Stat::ARMOR:
      return "ARMOR";
    case Stat::HEALTH:
      return "HEALTH";
    case Stat::MANA:
      return "MANA";
    case Stat::ATTACK_POWER:
      return "ATTACK_POWER";
    case Stat::RANGED_ATTACK_POWER:
      return "RANGED_ATTACK_POWER";
    case Stat::SPELL_POWER:
      return "SPELL_POWER";
    case Stat::MP5:
      return "MP5";
    case Stat::HIT_RATING:
      return "HIT_RATING";
    case Stat::CRIT_RATING:
      return "CRIT_RATING";
    case Stat::HASTE_RATING:
      return "HASTE_RATING";
    case Stat::EXPERTISE_RATING:
      return "EXPERTISE_RATING";
    case Stat::ARMOR_PENETRATION_RATING:
      return "ARMOR_PENETRATION_RATING";
    case Stat::SPELL_PENETRATION:
      return "SPELL_PENETRATION";
    case Stat::DEFENSE_RATING:
      return "DEFENSE_RATING";
    case Stat::DODGE_RATING:
      return "DODGE_RATING";
    case Stat::PARRY_RATING:
      return "PARRY_RATING";
    case Stat::BLOCK_RATING:
      return "BLOCK_RATING";
    case Stat::BLOCK_VALUE:
      return "BLOCK_VALUE";
    case Stat::RESILIENCE_RATING:
      return "RESILIENCE_RATING";
    case Stat::FIRE_RESISTANCE:
      return "FIRE_RESISTANCE";
    case Stat::FROST_RESISTANCE:
      return "FROST_RESISTANCE";
    case Stat::NATURE_RESISTANCE:
      return "NATURE_RESISTANCE";
    case Stat::SHADOW_RESISTANCE:
      return "SHADOW_RESISTANCE";
    case Stat::ARCANE_RESISTANCE:
      return "ARCANE_RESISTANCE";
    case Stat::WEAPON_DPS:
      return "WEAPON_DPS";
    case Stat::WEAPON_MIN_DAMAGE:
      return "WEAPON_MIN_DAMAGE";
    case Stat::WEAPON_MAX_DAMAGE:
      return "WEAPON_MAX_DAMAGE";
    case Stat::WEAPON_SPEED:
      return "WEAPON_SPEED";
    case Stat::MELEE_CRIT_PCT:
      return "MELEE_CRIT_PCT";
    case Stat::RANGED_CRIT_PCT:
      return "RANGED_CRIT_PCT";
    case Stat::SPELL_CRIT_PCT:
      return "SPELL_CRIT_PCT";
    case Stat::MELEE_HIT_PCT:
      return "MELEE_HIT_PCT";
    case Stat::SPELL_HIT_PCT:
      return "SPELL_HIT_PCT";
    case Stat::HASTE_PCT:
      return "HASTE_PCT";
    case Stat::EXPERTISE:
      return "EXPERTISE";
    case Stat::DEFENSE:
      return "DEFENSE";
    case Stat::DODGE_PCT:
      return "DODGE_PCT";
    case Stat::PARRY_PCT:
      return "PARRY_PCT";
    case Stat::BLOCK_PCT:
      return "BLOCK_PCT";
  }
  return "";
}

std::string_view EnumName(Panel value) {
  switch (value) {
    case Panel::PANEL_UNSPECIFIED:
      return "PANEL_UNSPECIFIED";
    case Panel::MAP:
      return "MAP";
    case Panel::BAGS:
      return "BAGS";
    case Panel::CHARACTER:
      return "CHARACTER";
    case Panel::QUEST_LOG:
      return "QUEST_LOG";
    case Panel::SPELLBOOK:
      return "SPELLBOOK";
    case Panel::TALENTS:
      return "TALENTS";
    case Panel::SKILLS:
      return "SKILLS";
    case Panel::REPUTATION:
      return "REPUTATION";
    case Panel::SOCIAL:
      return "SOCIAL";
    case Panel::CALENDAR:
      return "CALENDAR";
    case Panel::ACHIEVEMENTS:
      return "ACHIEVEMENTS";
    case Panel::CURRENCY:
      return "CURRENCY";
    case Panel::PVP:
      return "PVP";
    case Panel::COMPANIONS:
      return "COMPANIONS";
    case Panel::DUNGEON_FINDER:
      return "DUNGEON_FINDER";
    case Panel::SCOREBOARD:
      return "SCOREBOARD";
  }
  return "";
}

std::string_view EnumName(ChatChannel value) {
  switch (value) {
    case ChatChannel::CHAT_CHANNEL_UNSPECIFIED:
      return "CHAT_CHANNEL_UNSPECIFIED";
    case ChatChannel::SAY:
      return "SAY";
    case ChatChannel::YELL:
      return "YELL";
    case ChatChannel::WHISPER:
      return "WHISPER";
    case ChatChannel::WHISPER_SENT:
      return "WHISPER_SENT";
    case ChatChannel::PARTY:
      return "PARTY";
    case ChatChannel::RAID:
      return "RAID";
    case ChatChannel::RAID_WARNING:
      return "RAID_WARNING";
    case ChatChannel::GUILD:
      return "GUILD";
    case ChatChannel::OFFICER:
      return "OFFICER";
    case ChatChannel::GENERAL:
      return "GENERAL";
    case ChatChannel::TRADE:
      return "TRADE";
    case ChatChannel::LFG:
      return "LFG";
    case ChatChannel::EMOTE:
      return "EMOTE";
    case ChatChannel::MONSTER_SAY:
      return "MONSTER_SAY";
    case ChatChannel::MONSTER_YELL:
      return "MONSTER_YELL";
    case ChatChannel::MONSTER_EMOTE:
      return "MONSTER_EMOTE";
    case ChatChannel::MONSTER_WHISPER:
      return "MONSTER_WHISPER";
    case ChatChannel::BOSS_EMOTE:
      return "BOSS_EMOTE";
    case ChatChannel::SYSTEM:
      return "SYSTEM";
    case ChatChannel::CUSTOM_CHANNEL:
      return "CUSTOM_CHANNEL";
    case ChatChannel::BATTLEGROUND:
      return "BATTLEGROUND";
    case ChatChannel::ACHIEVEMENT:
      return "ACHIEVEMENT";
  }
  return "";
}

std::string_view EnumName(Self_Indicators_Durability::Level value) {
  switch (value) {
    case Self_Indicators_Durability::Level::LEVEL_UNSPECIFIED:
      return "LEVEL_UNSPECIFIED";
    case Self_Indicators_Durability::Level::DAMAGED:
      return "DAMAGED";
    case Self_Indicators_Durability::Level::BROKEN:
      return "BROKEN";
  }
  return "";
}

std::string_view EnumName(GoalPart::Kind value) {
  switch (value) {
    case GoalPart::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case GoalPart::Kind::SELL:
      return "SELL";
    case GoalPart::Kind::BUY:
      return "BUY";
    case GoalPart::Kind::TRAIN:
      return "TRAIN";
    case GoalPart::Kind::REPAIR:
      return "REPAIR";
    case GoalPart::Kind::LEARN_PROFESSION:
      return "LEARN_PROFESSION";
    case GoalPart::Kind::REACH_LEVEL:
      return "REACH_LEVEL";
    case GoalPart::Kind::TRAVEL_TO:
      return "TRAVEL_TO";
    case GoalPart::Kind::MEET:
      return "MEET";
    case GoalPart::Kind::FARM:
      return "FARM";
    case GoalPart::Kind::OTHER:
      return "OTHER";
  }
  return "";
}

std::string_view EnumName(HoverRef::Kind value) {
  switch (value) {
    case HoverRef::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case HoverRef::Kind::ITEM:
      return "ITEM";
    case HoverRef::Kind::SPELL:
      return "SPELL";
    case HoverRef::Kind::AURA:
      return "AURA";
    case HoverRef::Kind::UNIT:
      return "UNIT";
    case HoverRef::Kind::ZONE:
      return "ZONE";
    case HoverRef::Kind::PLACE:
      return "PLACE";
    case HoverRef::Kind::QUEST:
      return "QUEST";
    case HoverRef::Kind::ACHIEVEMENT:
      return "ACHIEVEMENT";
    case HoverRef::Kind::OBJECT:
      return "OBJECT";
  }
  return "";
}

std::string_view EnumName(Calendar_Entry::Status value) {
  switch (value) {
    case Calendar_Entry::Status::STATUS_UNSPECIFIED:
      return "STATUS_UNSPECIFIED";
    case Calendar_Entry::Status::INVITED:
      return "INVITED";
    case Calendar_Entry::Status::ACCEPTED:
      return "ACCEPTED";
    case Calendar_Entry::Status::DECLINED:
      return "DECLINED";
    case Calendar_Entry::Status::TENTATIVE:
      return "TENTATIVE";
  }
  return "";
}

std::string_view EnumName(DisplacedEvent::How value) {
  switch (value) {
    case DisplacedEvent::How::HOW_UNSPECIFIED:
      return "HOW_UNSPECIFIED";
    case DisplacedEvent::How::KNOCKED_BACK:
      return "KNOCKED_BACK";
    case DisplacedEvent::How::PULLED:
      return "PULLED";
    case DisplacedEvent::How::TELEPORTED:
      return "TELEPORTED";
  }
  return "";
}

std::string_view EnumName(Blank::Kind value) {
  switch (value) {
    case Blank::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case Blank::Kind::TEXT:
      return "TEXT";
    case Blank::Kind::NEW_NAME:
      return "NEW_NAME";
    case Blank::Kind::PLAYER:
      return "PLAYER";
    case Blank::Kind::MONEY:
      return "MONEY";
    case Blank::Kind::COUNT:
      return "COUNT";
  }
  return "";
}

std::string_view EnumName(Relative::Height value) {
  switch (value) {
    case Relative::Height::HEIGHT_UNSPECIFIED:
      return "HEIGHT_UNSPECIFIED";
    case Relative::Height::LEVEL:
      return "LEVEL";
    case Relative::Height::ABOVE:
      return "ABOVE";
    case Relative::Height::BELOW:
      return "BELOW";
  }
  return "";
}

std::string_view EnumName(Spell::School value) {
  switch (value) {
    case Spell::School::SCHOOL_UNSPECIFIED:
      return "SCHOOL_UNSPECIFIED";
    case Spell::School::PHYSICAL:
      return "PHYSICAL";
    case Spell::School::HOLY:
      return "HOLY";
    case Spell::School::FIRE:
      return "FIRE";
    case Spell::School::NATURE:
      return "NATURE";
    case Spell::School::FROST:
      return "FROST";
    case Spell::School::SHADOW:
      return "SHADOW";
    case Spell::School::ARCANE:
      return "ARCANE";
  }
  return "";
}

std::string_view EnumName(Item::Quality value) {
  switch (value) {
    case Item::Quality::QUALITY_UNSPECIFIED:
      return "QUALITY_UNSPECIFIED";
    case Item::Quality::POOR:
      return "POOR";
    case Item::Quality::COMMON:
      return "COMMON";
    case Item::Quality::UNCOMMON:
      return "UNCOMMON";
    case Item::Quality::RARE_QUALITY:
      return "RARE_QUALITY";
    case Item::Quality::EPIC:
      return "EPIC";
    case Item::Quality::LEGENDARY:
      return "LEGENDARY";
    case Item::Quality::ARTIFACT:
      return "ARTIFACT";
    case Item::Quality::HEIRLOOM:
      return "HEIRLOOM";
  }
  return "";
}

std::string_view EnumName(Item::Kind value) {
  switch (value) {
    case Item::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case Item::Kind::EQUIPMENT:
      return "EQUIPMENT";
    case Item::Kind::CONSUMABLE:
      return "CONSUMABLE";
    case Item::Kind::QUEST:
      return "QUEST";
    case Item::Kind::TRADE_GOOD:
      return "TRADE_GOOD";
    case Item::Kind::RECIPE:
      return "RECIPE";
    case Item::Kind::CONTAINER:
      return "CONTAINER";
    case Item::Kind::REAGENT:
      return "REAGENT";
    case Item::Kind::PROJECTILE:
      return "PROJECTILE";
    case Item::Kind::JUNK:
      return "JUNK";
    case Item::Kind::KEY:
      return "KEY";
    case Item::Kind::GEM:
      return "GEM";
    case Item::Kind::GLYPH:
      return "GLYPH";
    case Item::Kind::CURRENCY_TOKEN:
      return "CURRENCY_TOKEN";
    case Item::Kind::OTHER:
      return "OTHER";
  }
  return "";
}

std::string_view EnumName(Item::Binding value) {
  switch (value) {
    case Item::Binding::BINDING_UNSPECIFIED:
      return "BINDING_UNSPECIFIED";
    case Item::Binding::NOT_BOUND:
      return "NOT_BOUND";
    case Item::Binding::SOULBOUND:
      return "SOULBOUND";
    case Item::Binding::BIND_ON_PICKUP:
      return "BIND_ON_PICKUP";
    case Item::Binding::BIND_ON_EQUIP:
      return "BIND_ON_EQUIP";
    case Item::Binding::BIND_ON_USE:
      return "BIND_ON_USE";
    case Item::Binding::BIND_TO_ACCOUNT:
      return "BIND_TO_ACCOUNT";
  }
  return "";
}

std::string_view EnumName(PetPart::Happiness value) {
  switch (value) {
    case PetPart::Happiness::HAPPINESS_UNSPECIFIED:
      return "HAPPINESS_UNSPECIFIED";
    case PetPart::Happiness::UNHAPPY:
      return "UNHAPPY";
    case PetPart::Happiness::CONTENT:
      return "CONTENT";
    case PetPart::Happiness::HAPPY:
      return "HAPPY";
  }
  return "";
}

std::string_view EnumName(GameObject::Kind value) {
  switch (value) {
    case GameObject::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case GameObject::Kind::QUEST_OBJECT:
      return "QUEST_OBJECT";
    case GameObject::Kind::HERB:
      return "HERB";
    case GameObject::Kind::ORE:
      return "ORE";
    case GameObject::Kind::CHEST:
      return "CHEST";
    case GameObject::Kind::MAILBOX:
      return "MAILBOX";
    case GameObject::Kind::DOOR:
      return "DOOR";
    case GameObject::Kind::CAMPFIRE:
      return "CAMPFIRE";
    case GameObject::Kind::BOBBER:
      return "BOBBER";
    case GameObject::Kind::MEETING_STONE:
      return "MEETING_STONE";
    case GameObject::Kind::SUMMONING_PORTAL:
      return "SUMMONING_PORTAL";
    case GameObject::Kind::TRANSPORT:
      return "TRANSPORT";
    case GameObject::Kind::FISHING_POOL:
      return "FISHING_POOL";
    case GameObject::Kind::BARBER_CHAIR:
      return "BARBER_CHAIR";
    case GameObject::Kind::GUILD_VAULT:
      return "GUILD_VAULT";
    case GameObject::Kind::PORTAL:
      return "PORTAL";
    case GameObject::Kind::RITUAL:
      return "RITUAL";
    case GameObject::Kind::OTHER:
      return "OTHER";
    case GameObject::Kind::CAPTURE_POINT:
      return "CAPTURE_POINT";
    case GameObject::Kind::DESTRUCTIBLE:
      return "DESTRUCTIBLE";
  }
  return "";
}

std::string_view EnumName(Place::Kind value) {
  switch (value) {
    case Place::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case Place::Kind::TOWN:
      return "TOWN";
    case Place::Kind::CAMP:
      return "CAMP";
    case Place::Kind::FLIGHT_MASTER:
      return "FLIGHT_MASTER";
    case Place::Kind::INNKEEPER:
      return "INNKEEPER";
    case Place::Kind::GRAVEYARD:
      return "GRAVEYARD";
    case Place::Kind::DUNGEON:
      return "DUNGEON";
    case Place::Kind::RAID:
      return "RAID";
    case Place::Kind::ZONE_EXIT:
      return "ZONE_EXIT";
    case Place::Kind::LANDMARK:
      return "LANDMARK";
    case Place::Kind::CAMPFIRE:
      return "CAMPFIRE";
    case Place::Kind::AUCTION_HOUSE:
      return "AUCTION_HOUSE";
    case Place::Kind::BANK:
      return "BANK";
    case Place::Kind::TRAINER:
      return "TRAINER";
    case Place::Kind::VENDOR:
      return "VENDOR";
    case Place::Kind::DOCK:
      return "DOCK";
    case Place::Kind::ZEPPELIN_TOWER:
      return "ZEPPELIN_TOWER";
    case Place::Kind::ZONE:
      return "ZONE";
    case Place::Kind::OBJECTIVE_AREA:
      return "OBJECTIVE_AREA";
    case Place::Kind::BATTLEMASTER:
      return "BATTLEMASTER";
    case Place::Kind::GUILD_BANK:
      return "GUILD_BANK";
    case Place::Kind::BARBER_SHOP:
      return "BARBER_SHOP";
    case Place::Kind::STABLE:
      return "STABLE";
  }
  return "";
}

std::string_view EnumName(Surroundings_Obstacle::Kind value) {
  switch (value) {
    case Surroundings_Obstacle::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case Surroundings_Obstacle::Kind::WALL:
      return "WALL";
    case Surroundings_Obstacle::Kind::SLOPE:
      return "SLOPE";
    case Surroundings_Obstacle::Kind::DROP:
      return "DROP";
    case Surroundings_Obstacle::Kind::WATER:
      return "WATER";
    case Surroundings_Obstacle::Kind::LEDGE:
      return "LEDGE";
    case Surroundings_Obstacle::Kind::OPENING:
      return "OPENING";
    case Surroundings_Obstacle::Kind::DOOR:
      return "DOOR";
    case Surroundings_Obstacle::Kind::TREE:
      return "TREE";
    case Surroundings_Obstacle::Kind::ROCK:
      return "ROCK";
  }
  return "";
}

std::string_view EnumName(PromptEvent::Kind value) {
  switch (value) {
    case PromptEvent::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case PromptEvent::Kind::GROUP_INVITE:
      return "GROUP_INVITE";
    case PromptEvent::Kind::TRADE:
      return "TRADE";
    case PromptEvent::Kind::DUEL:
      return "DUEL";
    case PromptEvent::Kind::GUILD_INVITE:
      return "GUILD_INVITE";
    case PromptEvent::Kind::READY_CHECK:
      return "READY_CHECK";
    case PromptEvent::Kind::ROLE_CHECK:
      return "ROLE_CHECK";
    case PromptEvent::Kind::RESURRECT_OFFER:
      return "RESURRECT_OFFER";
    case PromptEvent::Kind::SUMMON:
      return "SUMMON";
    case PromptEvent::Kind::QUEST_SHARE:
      return "QUEST_SHARE";
    case PromptEvent::Kind::BATTLEGROUND_READY:
      return "BATTLEGROUND_READY";
    case PromptEvent::Kind::ARENA_TEAM_INVITE:
      return "ARENA_TEAM_INVITE";
    case PromptEvent::Kind::WINTERGRASP_INVITE:
      return "WINTERGRASP_INVITE";
    case PromptEvent::Kind::BIND_HEARTHSTONE:
      return "BIND_HEARTHSTONE";
    case PromptEvent::Kind::CONFIRM_BIND_ON_PICKUP:
      return "CONFIRM_BIND_ON_PICKUP";
    case PromptEvent::Kind::CONFIRM_DELETE_ITEM:
      return "CONFIRM_DELETE_ITEM";
    case PromptEvent::Kind::CONFIRM_ABANDON_QUEST:
      return "CONFIRM_ABANDON_QUEST";
    case PromptEvent::Kind::CONFIRM_REPLACE_ENCHANT:
      return "CONFIRM_REPLACE_ENCHANT";
    case PromptEvent::Kind::CONFIRM_TALENT_WIPE:
      return "CONFIRM_TALENT_WIPE";
    case PromptEvent::Kind::CONFIRM_RELEASE_SPIRIT:
      return "CONFIRM_RELEASE_SPIRIT";
    case PromptEvent::Kind::CONFIRM_LOOT_ROLL:
      return "CONFIRM_LOOT_ROLL";
    case PromptEvent::Kind::CONFIRM_PETITION_SIGN:
      return "CONFIRM_PETITION_SIGN";
    case PromptEvent::Kind::CONFIRM_LEAVE_BATTLEGROUND:
      return "CONFIRM_LEAVE_BATTLEGROUND";
    case PromptEvent::Kind::CONFIRM_INSTANCE_LOCK:
      return "CONFIRM_INSTANCE_LOCK";
    case PromptEvent::Kind::CONFIRM_SPIRIT_HEALER:
      return "CONFIRM_SPIRIT_HEALER";
    case PromptEvent::Kind::CONFIRM_GUILD_LEADER:
      return "CONFIRM_GUILD_LEADER";
    case PromptEvent::Kind::CONFIRM_REPLACE_GEM:
      return "CONFIRM_REPLACE_GEM";
    case PromptEvent::Kind::LFG_PROPOSAL:
      return "LFG_PROPOSAL";
    case PromptEvent::Kind::LFG_BOOT_VOTE:
      return "LFG_BOOT_VOTE";
    case PromptEvent::Kind::LFG_OFFER_CONTINUE:
      return "LFG_OFFER_CONTINUE";
    case PromptEvent::Kind::CONFIRM_PURCHASE:
      return "CONFIRM_PURCHASE";
    case PromptEvent::Kind::SPIRIT_HEALER_WAVE:
      return "SPIRIT_HEALER_WAVE";
    case PromptEvent::Kind::RECLAIM_CORPSE:
      return "RECLAIM_CORPSE";
    case PromptEvent::Kind::CHANNEL_INVITE:
      return "CHANNEL_INVITE";
    case PromptEvent::Kind::CHANNEL_PASSWORD:
      return "CHANNEL_PASSWORD";
    case PromptEvent::Kind::PET_NAME:
      return "PET_NAME";
  }
  return "";
}

std::string_view EnumName(Activity::State value) {
  switch (value) {
    case Activity::State::STATE_UNSPECIFIED:
      return "STATE_UNSPECIFIED";
    case Activity::State::IDLE:
      return "IDLE";
    case Activity::State::MOVING:
      return "MOVING";
    case Activity::State::PATROLLING:
      return "PATROLLING";
    case Activity::State::APPROACHING_ME:
      return "APPROACHING_ME";
    case Activity::State::FLEEING:
      return "FLEEING";
    case Activity::State::FIGHTING:
      return "FIGHTING";
    case Activity::State::CASTING:
      return "CASTING";
    case Activity::State::CHANNELING:
      return "CHANNELING";
    case Activity::State::TALKING:
      return "TALKING";
    case Activity::State::EMOTING:
      return "EMOTING";
    case Activity::State::LOOTING:
      return "LOOTING";
    case Activity::State::GATHERING:
      return "GATHERING";
    case Activity::State::SITTING:
      return "SITTING";
    case Activity::State::FOLLOWING:
      return "FOLLOWING";
    case Activity::State::FISHING:
      return "FISHING";
    case Activity::State::DEAD:
      return "DEAD";
    case Activity::State::DRINKING:
      return "DRINKING";
    case Activity::State::STEALTHED:
      return "STEALTHED";
    case Activity::State::AFK:
      return "AFK";
  }
  return "";
}

std::string_view EnumName(EquipmentPart::Slot value) {
  switch (value) {
    case EquipmentPart::Slot::SLOT_UNSPECIFIED:
      return "SLOT_UNSPECIFIED";
    case EquipmentPart::Slot::HEAD:
      return "HEAD";
    case EquipmentPart::Slot::NECK:
      return "NECK";
    case EquipmentPart::Slot::SHOULDER:
      return "SHOULDER";
    case EquipmentPart::Slot::BACK:
      return "BACK";
    case EquipmentPart::Slot::CHEST:
      return "CHEST";
    case EquipmentPart::Slot::SHIRT:
      return "SHIRT";
    case EquipmentPart::Slot::TABARD:
      return "TABARD";
    case EquipmentPart::Slot::WRIST:
      return "WRIST";
    case EquipmentPart::Slot::HANDS:
      return "HANDS";
    case EquipmentPart::Slot::WAIST:
      return "WAIST";
    case EquipmentPart::Slot::LEGS:
      return "LEGS";
    case EquipmentPart::Slot::FEET:
      return "FEET";
    case EquipmentPart::Slot::FINGER1:
      return "FINGER1";
    case EquipmentPart::Slot::FINGER2:
      return "FINGER2";
    case EquipmentPart::Slot::TRINKET1:
      return "TRINKET1";
    case EquipmentPart::Slot::TRINKET2:
      return "TRINKET2";
    case EquipmentPart::Slot::MAIN_HAND:
      return "MAIN_HAND";
    case EquipmentPart::Slot::OFF_HAND:
      return "OFF_HAND";
    case EquipmentPart::Slot::RANGED:
      return "RANGED";
    case EquipmentPart::Slot::BAG:
      return "BAG";
  }
  return "";
}

std::string_view EnumName(Unit::Kind value) {
  switch (value) {
    case Unit::Kind::KIND_UNSPECIFIED:
      return "KIND_UNSPECIFIED";
    case Unit::Kind::NPC:
      return "NPC";
    case Unit::Kind::PLAYER:
      return "PLAYER";
    case Unit::Kind::PET:
      return "PET";
    case Unit::Kind::VEHICLE:
      return "VEHICLE";
    case Unit::Kind::TOTEM:
      return "TOTEM";
    case Unit::Kind::CRITTER:
      return "CRITTER";
  }
  return "";
}

std::string_view EnumName(Unit::LevelColor value) {
  switch (value) {
    case Unit::LevelColor::LEVEL_COLOR_UNSPECIFIED:
      return "LEVEL_COLOR_UNSPECIFIED";
    case Unit::LevelColor::GRAY:
      return "GRAY";
    case Unit::LevelColor::GREEN:
      return "GREEN";
    case Unit::LevelColor::YELLOW:
      return "YELLOW";
    case Unit::LevelColor::ORANGE:
      return "ORANGE";
    case Unit::LevelColor::RED:
      return "RED";
    case Unit::LevelColor::SKULL_LEVEL:
      return "SKULL_LEVEL";
  }
  return "";
}

std::string_view EnumName(Unit::Reaction value) {
  switch (value) {
    case Unit::Reaction::REACTION_UNSPECIFIED:
      return "REACTION_UNSPECIFIED";
    case Unit::Reaction::HOSTILE:
      return "HOSTILE";
    case Unit::Reaction::NEUTRAL:
      return "NEUTRAL";
    case Unit::Reaction::FRIENDLY:
      return "FRIENDLY";
  }
  return "";
}

std::string_view EnumName(Unit::Classification value) {
  switch (value) {
    case Unit::Classification::CLASSIFICATION_UNSPECIFIED:
      return "CLASSIFICATION_UNSPECIFIED";
    case Unit::Classification::NORMAL:
      return "NORMAL";
    case Unit::Classification::ELITE:
      return "ELITE";
    case Unit::Classification::RARE:
      return "RARE";
    case Unit::Classification::RARE_ELITE:
      return "RARE_ELITE";
    case Unit::Classification::WORLD_BOSS:
      return "WORLD_BOSS";
  }
  return "";
}

std::string_view EnumName(Unit::ThreatStatus value) {
  switch (value) {
    case Unit::ThreatStatus::THREAT_STATUS_UNSPECIFIED:
      return "THREAT_STATUS_UNSPECIFIED";
    case Unit::ThreatStatus::THREAT_NONE:
      return "THREAT_NONE";
    case Unit::ThreatStatus::THREAT_HIGH:
      return "THREAT_HIGH";
    case Unit::ThreatStatus::THREAT_AGGRO:
      return "THREAT_AGGRO";
  }
  return "";
}

std::string_view EnumName(Unit::QuestMarker value) {
  switch (value) {
    case Unit::QuestMarker::QUEST_MARKER_UNSPECIFIED:
      return "QUEST_MARKER_UNSPECIFIED";
    case Unit::QuestMarker::QUEST_AVAILABLE:
      return "QUEST_AVAILABLE";
    case Unit::QuestMarker::QUEST_AVAILABLE_LOW:
      return "QUEST_AVAILABLE_LOW";
    case Unit::QuestMarker::QUEST_TURN_IN:
      return "QUEST_TURN_IN";
    case Unit::QuestMarker::QUEST_INCOMPLETE:
      return "QUEST_INCOMPLETE";
    case Unit::QuestMarker::QUEST_DAILY:
      return "QUEST_DAILY";
  }
  return "";
}

std::string_view EnumName(Unit::Service value) {
  switch (value) {
    case Unit::Service::SERVICE_UNSPECIFIED:
      return "SERVICE_UNSPECIFIED";
    case Unit::Service::VENDOR:
      return "VENDOR";
    case Unit::Service::CLASS_TRAINER:
      return "CLASS_TRAINER";
    case Unit::Service::PROFESSION_TRAINER:
      return "PROFESSION_TRAINER";
    case Unit::Service::REPAIR:
      return "REPAIR";
    case Unit::Service::INNKEEPER:
      return "INNKEEPER";
    case Unit::Service::FLIGHT_MASTER:
      return "FLIGHT_MASTER";
    case Unit::Service::BANKER:
      return "BANKER";
    case Unit::Service::AUCTIONEER:
      return "AUCTIONEER";
    case Unit::Service::STABLE_MASTER:
      return "STABLE_MASTER";
    case Unit::Service::SPIRIT_HEALER:
      return "SPIRIT_HEALER";
    case Unit::Service::BATTLEMASTER:
      return "BATTLEMASTER";
    case Unit::Service::GUILD_MASTER:
      return "GUILD_MASTER";
    case Unit::Service::TABARD_VENDOR:
      return "TABARD_VENDOR";
    case Unit::Service::QUEST_GIVER:
      return "QUEST_GIVER";
    case Unit::Service::GUILD_BANKER:
      return "GUILD_BANKER";
    case Unit::Service::BARBER:
      return "BARBER";
    case Unit::Service::ARENA_ORGANIZER:
      return "ARENA_ORGANIZER";
    case Unit::Service::WEAPON_MASTER:
      return "WEAPON_MASTER";
  }
  return "";
}

std::string_view EnumName(Unit::VisualState value) {
  switch (value) {
    case Unit::VisualState::VISUAL_STATE_UNSPECIFIED:
      return "VISUAL_STATE_UNSPECIFIED";
    case Unit::VisualState::POLYMORPHED:
      return "POLYMORPHED";
    case Unit::VisualState::STUNNED:
      return "STUNNED";
    case Unit::VisualState::FEARED:
      return "FEARED";
    case Unit::VisualState::SAPPED:
      return "SAPPED";
    case Unit::VisualState::ASLEEP:
      return "ASLEEP";
    case Unit::VisualState::BANISHED:
      return "BANISHED";
    case Unit::VisualState::FROZEN:
      return "FROZEN";
    case Unit::VisualState::ICE_BLOCK:
      return "ICE_BLOCK";
    case Unit::VisualState::BUBBLED:
      return "BUBBLED";
    case Unit::VisualState::MIND_CONTROLLED:
      return "MIND_CONTROLLED";
    case Unit::VisualState::STEALTH_SHIMMER:
      return "STEALTH_SHIMMER";
    case Unit::VisualState::SHACKLED:
      return "SHACKLED";
    case Unit::VisualState::ROOTED:
      return "ROOTED";
    case Unit::VisualState::DRUNK_SWAY:
      return "DRUNK_SWAY";
  }
  return "";
}

std::string_view EnumName(Unit::Size value) {
  switch (value) {
    case Unit::Size::SIZE_UNSPECIFIED:
      return "SIZE_UNSPECIFIED";
    case Unit::Size::SMALL:
      return "SMALL";
    case Unit::Size::MEDIUM:
      return "MEDIUM";
    case Unit::Size::LARGE:
      return "LARGE";
    case Unit::Size::HUGE:
      return "HUGE";
  }
  return "";
}

std::string_view EnumName(Unit::Detail value) {
  switch (value) {
    case Unit::Detail::DETAIL_UNSPECIFIED:
      return "DETAIL_UNSPECIFIED";
    case Unit::Detail::NAMEPLATE:
      return "NAMEPLATE";
    case Unit::Detail::MODEL:
      return "MODEL";
  }
  return "";
}

std::string_view EnumName(Self::Gender value) {
  switch (value) {
    case Self::Gender::GENDER_UNSPECIFIED:
      return "GENDER_UNSPECIFIED";
    case Self::Gender::MALE:
      return "MALE";
    case Self::Gender::FEMALE:
      return "FEMALE";
  }
  return "";
}

std::string_view EnumName(Self::PowerType value) {
  switch (value) {
    case Self::PowerType::POWER_TYPE_UNSPECIFIED:
      return "POWER_TYPE_UNSPECIFIED";
    case Self::PowerType::MANA:
      return "MANA";
    case Self::PowerType::RAGE:
      return "RAGE";
    case Self::PowerType::ENERGY:
      return "ENERGY";
    case Self::PowerType::RUNIC_POWER:
      return "RUNIC_POWER";
    case Self::PowerType::FOCUS:
      return "FOCUS";
  }
  return "";
}

std::string_view EnumName(Self::LifeState value) {
  switch (value) {
    case Self::LifeState::LIFE_STATE_UNSPECIFIED:
      return "LIFE_STATE_UNSPECIFIED";
    case Self::LifeState::ALIVE:
      return "ALIVE";
    case Self::LifeState::DEAD_BODY:
      return "DEAD_BODY";
    case Self::LifeState::GHOST:
      return "GHOST";
  }
  return "";
}

std::string_view EnumName(Self::Drunk value) {
  switch (value) {
    case Self::Drunk::DRUNK_UNSPECIFIED:
      return "DRUNK_UNSPECIFIED";
    case Self::Drunk::SOBER:
      return "SOBER";
    case Self::Drunk::TIPSY:
      return "TIPSY";
    case Self::Drunk::DRUNK:
      return "DRUNK";
    case Self::Drunk::SMASHED:
      return "SMASHED";
  }
  return "";
}

}  // namespace wlb::snapshot
