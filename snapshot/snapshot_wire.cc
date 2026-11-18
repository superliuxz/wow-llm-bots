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

std::string WriteJson(const OptionFacts& facts, bool pretty) {
  std::string out;
  const auto ec = pretty ? glz::write<PrettyJsonWriteOpts{}>(facts, out)
                         : glz::write<CompactJsonWriteOpts{}>(facts, out);
  if (ec) {
    return "";
  }
  return out;
}

bool ReadJson(std::string_view json, OptionFacts* out, std::string* error) {
  *out = OptionFacts{};
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
