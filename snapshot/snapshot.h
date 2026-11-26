// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Module <-> harness snapshot and command protocol (plain C++20 structs).
// Wire serialization (BEVE) and JSON mapping live in snapshot_wire.cc (Glaze).
// Keep this header pure C++20: AzerothCore compiles server_module as C++20.

#ifndef WLB_SNAPSHOT_SNAPSHOT_H_
#define WLB_SNAPSHOT_SNAPSHOT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wlb::snapshot {

// Value-semantics heap box used to break the Item <-> EquipmentPart::Socket
// recursion while preserving copyability, equality, and Glaze nullable support.
template <typename T>
class Box {
 public:
  using value_type = T;
  Box() noexcept = default;
  ~Box();
  Box(const Box& other);
  Box(Box&&) noexcept = default;
  Box& operator=(const Box& other);
  Box& operator=(Box&&) noexcept = default;

  bool has_value() const noexcept { return ptr_ != nullptr; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
  T& value() { return *ptr_; }
  const T& value() const { return *ptr_; }
  T* operator->() { return ptr_.get(); }
  const T* operator->() const { return ptr_.get(); }
  template <typename... Args>
  T& emplace(Args&&... args) {
    ptr_ = std::make_unique<T>(std::forward<Args>(args)...);
    return *ptr_;
  }
  void reset() noexcept { ptr_.reset(); }
  bool operator==(const Box& other) const;

 private:
  std::unique_ptr<T> ptr_;
};

enum class Direction : uint8_t {
  DIRECTION_UNSPECIFIED = 0,
  HERE = 1,
  N = 2,
  NE = 3,
  E = 4,
  SE = 5,
  S = 6,
  SW = 7,
  W = 8,
  NW = 9,
};
using enum Direction;

enum class DispelType : uint8_t {
  DISPEL_TYPE_UNSPECIFIED = 0,
  DISPEL_NONE = 1,
  MAGIC = 2,
  CURSE = 3,
  POISON = 4,
  DISEASE = 5,
};
using enum DispelType;

enum class RaidMarker : uint8_t {
  RAID_MARKER_UNSPECIFIED = 0,
  STAR = 1,
  CIRCLE = 2,
  DIAMOND = 3,
  TRIANGLE = 4,
  MOON = 5,
  SQUARE = 6,
  CROSS = 7,
  SKULL = 8,
};
using enum RaidMarker;

enum class Role : uint8_t {
  ROLE_UNSPECIFIED = 0,
  TANK = 1,
  HEALER = 2,
  DPS = 3,
};
using enum Role;

enum class Stat : uint8_t {
  STAT_UNSPECIFIED = 0,
  STRENGTH = 1,
  AGILITY = 2,
  STAMINA = 3,
  INTELLECT = 4,
  SPIRIT = 5,
  ARMOR = 6,
  HEALTH = 7,
  MANA = 8,
  ATTACK_POWER = 9,
  RANGED_ATTACK_POWER = 10,
  SPELL_POWER = 11,
  MP5 = 12,
  HIT_RATING = 13,
  CRIT_RATING = 14,
  HASTE_RATING = 15,
  EXPERTISE_RATING = 16,
  ARMOR_PENETRATION_RATING = 17,
  SPELL_PENETRATION = 18,
  DEFENSE_RATING = 19,
  DODGE_RATING = 20,
  PARRY_RATING = 21,
  BLOCK_RATING = 22,
  BLOCK_VALUE = 23,
  RESILIENCE_RATING = 24,
  FIRE_RESISTANCE = 25,
  FROST_RESISTANCE = 26,
  NATURE_RESISTANCE = 27,
  SHADOW_RESISTANCE = 28,
  ARCANE_RESISTANCE = 29,
  WEAPON_DPS = 30,
  WEAPON_MIN_DAMAGE = 31,
  WEAPON_MAX_DAMAGE = 32,
  WEAPON_SPEED = 33,
  MELEE_CRIT_PCT = 40,
  RANGED_CRIT_PCT = 41,
  SPELL_CRIT_PCT = 42,
  MELEE_HIT_PCT = 43,
  SPELL_HIT_PCT = 44,
  HASTE_PCT = 45,
  EXPERTISE = 46,
  DEFENSE = 47,
  DODGE_PCT = 48,
  PARRY_PCT = 49,
  BLOCK_PCT = 50,
};
using enum Stat;

enum class Panel : uint8_t {
  PANEL_UNSPECIFIED = 0,
  MAP = 1,
  BAGS = 2,
  CHARACTER = 3,
  QUEST_LOG = 4,
  SPELLBOOK = 5,
  TALENTS = 6,
  SKILLS = 7,
  REPUTATION = 8,
  SOCIAL = 9,
  CALENDAR = 10,
  ACHIEVEMENTS = 11,
  CURRENCY = 12,
  PVP = 13,
  COMPANIONS = 14,
  DUNGEON_FINDER = 15,
  SCOREBOARD = 16,
};
using enum Panel;

enum class ChatChannel : uint8_t {
  CHAT_CHANNEL_UNSPECIFIED = 0,
  SAY = 1,
  YELL = 2,
  WHISPER = 3,
  WHISPER_SENT = 4,
  PARTY = 5,
  RAID = 6,
  RAID_WARNING = 7,
  GUILD = 8,
  OFFICER = 9,
  GENERAL = 10,
  TRADE = 11,
  LFG = 12,
  EMOTE = 13,
  MONSTER_SAY = 14,
  MONSTER_YELL = 15,
  MONSTER_EMOTE = 16,
  MONSTER_WHISPER = 17,
  BOSS_EMOTE = 18,
  SYSTEM = 19,
  CUSTOM_CHANNEL = 20,
  BATTLEGROUND = 21,
  ACHIEVEMENT = 22,
};
using enum ChatChannel;

struct EquipmentPart;

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  bool operator==(const Vec3&) const = default;
};

struct LevelRange {
  uint32_t min = 0;
  uint32_t max = 0;

  bool operator==(const LevelRange&) const = default;
};

struct Tooltip {
  std::vector<std::string> lines;

  bool operator==(const Tooltip&) const = default;
};

struct CurrencyAmount {
  std::string name;
  uint32_t count = 0;

  bool operator==(const CurrencyAmount&) const = default;
};

struct StatValue {
  Stat stat = Stat::STAT_UNSPECIFIED;
  float value = 0.0f;

  bool operator==(const StatValue&) const = default;
};

struct Spell_Cost {
  std::string power;
  uint32_t amount = 0;
  float pct = 0.0f;

  bool operator==(const Spell_Cost&) const = default;
};

struct AbilityState {
  float cooldown_remaining_s = 0.0f;
  bool usable = false;
  bool in_range = false;
  bool known = false;
  uint32_t charges = 0;
  std::string slot;
  uint32_t item_count = 0;
  bool autocast = false;
  bool ground_targeted = false;

  bool operator==(const AbilityState&) const = default;
};

struct TrainerSpell {
  uint32_t price = 0;
  uint32_t required_level = 0;
  bool learnable = false;
  bool learned = false;
  std::string requires_;

  bool operator==(const TrainerSpell&) const = default;
};

struct Unit_Pack {
  uint32_t id = 0;
  uint32_t size = 0;

  bool operator==(const Unit_Pack&) const = default;
};

struct PlayerPart {
  std::string class_;
  std::string race;   // visibility: HOVER
  std::string guild;  // visibility: HOVER
  std::string title;
  bool in_my_group = false;
  bool is_friend = false;
  bool is_ignored = false;
  bool afk = false;
  bool dnd = false;
  uint32_t gearscore = 0;  // visibility: HOVER, addon: GearScore

  bool operator==(const PlayerPart&) const = default;
};

struct GroupPart {
  Role role = Role::ROLE_UNSPECIFIED;
  bool online = false;
  uint32_t health_deficit = 0;
  bool in_range = false;
  bool aggro = false;
  uint32_t subgroup = 0;
  bool leader = false;
  bool assistant = false;
  bool main_tank = false;
  bool main_assist = false;
  // Flagged away (/afk), as the party frame shows.
  bool afk = false;
  std::string ready;
  // Unfinished quests of a party member in range, with their progress
  // ("Kobold Camp Cleanup: Kobold Vermin slain 3/8"), at most 3 (Now #8).
  std::vector<std::string> quests;

  bool operator==(const GroupPart&) const = default;
};

struct BossPart {
  uint32_t phase = 0;  // addon: DBM

  bool operator==(const BossPart&) const = default;
};

struct VehiclePart_Seat {
  uint32_t index = 0;
  std::string occupant;
  bool can_use = false;

  bool operator==(const VehiclePart_Seat&) const = default;
};

struct Self_Control {
  bool stunned = false;
  bool feared = false;
  bool charmed = false;
  bool silenced = false;
  bool rooted = false;
  bool pacified = false;
  bool disarmed = false;

  bool operator==(const Self_Control&) const = default;
};

struct Self_Indicators_Durability {
  enum class Level : uint8_t {
    LEVEL_UNSPECIFIED = 0,
    DAMAGED = 1,
    BROKEN = 2,
  };
  using enum Level;

  std::string slot;
  Self_Indicators_Durability::Level level =
      Self_Indicators_Durability::Level::LEVEL_UNSPECIFIED;

  bool operator==(const Self_Indicators_Durability&) const = default;
};

struct Self_AutoAttack {
  bool melee = false;
  bool ranged = false;

  bool operator==(const Self_AutoAttack&) const = default;
};

struct Self_WeaponEnchant {
  std::string slot;
  std::string name;
  float remaining_s = 0.0f;
  uint32_t charges = 0;

  bool operator==(const Self_WeaponEnchant&) const = default;
};

struct Self_Rune {
  std::string type;
  float ready_in_s = 0.0f;

  bool operator==(const Self_Rune&) const = default;
};

struct Objective_Progress {
  uint32_t have = 0;
  uint32_t need = 0;

  bool operator==(const Objective_Progress&) const = default;
};

struct GoalPart {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    SELL = 1,
    BUY = 2,
    TRAIN = 3,
    REPAIR = 4,
    LEARN_PROFESSION = 5,
    REACH_LEVEL = 6,
    TRAVEL_TO = 7,
    MEET = 8,
    FARM = 9,
    OTHER = 10,
  };
  using enum Kind;

  std::string id;
  GoalPart::Kind goal_kind = GoalPart::Kind::KIND_UNSPECIFIED;
  std::vector<std::pair<std::string, std::string>> params;

  bool operator==(const GoalPart&) const = default;
};

struct AchievementPart {
  uint32_t achievement_id = 0;

  bool operator==(const AchievementPart&) const = default;
};

struct Achievement_Criterion {
  std::string name;
  bool done = false;
  uint32_t have = 0;
  uint32_t need = 0;

  bool operator==(const Achievement_Criterion&) const = default;
};

struct Doing {
  std::string activity;
  float since_s = 0.0f;
  std::string detail;

  bool operator==(const Doing&) const = default;
};

struct HoverRef {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    ITEM = 1,
    SPELL = 2,
    AURA = 3,
    UNIT = 4,
    ZONE = 5,
    PLACE = 6,
    QUEST = 7,
    ACHIEVEMENT = 8,
    OBJECT = 9,
  };
  using enum Kind;

  HoverRef::Kind kind = HoverRef::Kind::KIND_UNSPECIFIED;
  std::string id;
  std::string name;

  bool operator==(const HoverRef&) const = default;
};

struct WorldState {
  std::string label;
  std::string value;
  float timer_s = 0.0f;

  bool operator==(const WorldState&) const = default;
};

struct World_Queue {
  std::string name;
  std::string status;
  float waited_s = 0.0f;
  float estimated_s = 0.0f;

  bool operator==(const World_Queue&) const = default;
};

struct Combat_Threat {
  std::string unit;
  float my_threat_pct = 0.0f;
  std::string top;

  bool operator==(const Combat_Threat&) const = default;
};

struct Combat_Resources {
  uint32_t combo_points = 0;
  uint32_t soul_shards = 0;
  uint32_t ammo = 0;

  bool operator==(const Combat_Resources&) const = default;
};

struct Encounter_Timer {
  std::string name;
  float remaining_s = 0.0f;

  bool operator==(const Encounter_Timer&) const = default;
};

struct Instance_Boss {
  std::string name;
  bool killed = false;

  bool operator==(const Instance_Boss&) const = default;
};

struct PetFrame_Totem {
  std::string name;
  std::string element;
  float remaining_s = 0.0f;

  bool operator==(const PetFrame_Totem&) const = default;
};

struct Bags_Hearthstone {
  bool ready = false;
  std::string bind;

  bool operator==(const Bags_Hearthstone&) const = default;
};

struct Character_EquipmentSet {
  std::string name;
  bool complete = false;

  bool operator==(const Character_EquipmentSet&) const = default;
};

struct ErrorEvent {
  std::string text;
  uint32_t command_id = 0;

  bool operator==(const ErrorEvent&) const = default;
};

struct InfoEvent {
  std::string text;

  bool operator==(const InfoEvent&) const = default;
};

struct LevelUpEvent {
  uint32_t level = 0;
  uint32_t health_gain = 0;
  uint32_t talent_points = 0;
  std::vector<std::pair<std::string, int32_t>> stat_gains;

  bool operator==(const LevelUpEvent&) const = default;
};

struct XpGainedEvent {
  uint32_t amount = 0;
  std::string from;
  uint32_t rested_bonus = 0;

  bool operator==(const XpGainedEvent&) const = default;
};

struct MoneyChangedEvent {
  int32_t delta = 0;
  std::string reason;

  bool operator==(const MoneyChangedEvent&) const = default;
};

struct QuestEvent {
  std::string quest;
  uint32_t xp = 0;
  uint32_t money = 0;

  bool operator==(const QuestEvent&) const = default;
};

struct DiedEvent {
  std::string killer;
  bool durability_loss = false;

  bool operator==(const DiedEvent&) const = default;
};

struct ResurrectedEvent {
  std::string by;
  bool resurrection_sickness = false;

  bool operator==(const ResurrectedEvent&) const = default;
};

struct DisplacedEvent {
  enum class How : uint8_t {
    HOW_UNSPECIFIED = 0,
    KNOCKED_BACK = 1,
    PULLED = 2,
    TELEPORTED = 3,
  };
  using enum How;

  DisplacedEvent::How how = DisplacedEvent::How::HOW_UNSPECIFIED;
  std::string by;
  std::string spell;
  float distance_yd = 0.0f;

  bool operator==(const DisplacedEvent&) const = default;
};

struct DuelEvent {
  std::string with;
  std::string winner;
  std::string what;

  bool operator==(const DuelEvent&) const = default;
};

struct KickedEvent {
  std::string from;
  std::string by;
  std::string reason;

  bool operator==(const KickedEvent&) const = default;
};

struct DbmWarningEvent {
  std::string text;

  bool operator==(const DbmWarningEvent&) const = default;
};

struct Blank {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    TEXT = 1,
    NEW_NAME = 2,
    PLAYER = 3,
    MONEY = 4,
    COUNT = 5,
  };
  using enum Kind;

  Blank::Kind kind = Blank::Kind::KIND_UNSPECIFIED;
  std::string label;
  uint32_t max_length = 0;
  uint32_t min = 0;
  uint32_t max = 0;

  bool operator==(const Blank&) const = default;
};

struct Command_Choose {
  uint32_t option = 0;
  uint32_t seq = 0;
  std::vector<std::string> fills;

  bool operator==(const Command_Choose&) const = default;
};

struct Command_ClearGoal {
  std::string id;

  bool operator==(const Command_ClearGoal&) const = default;
};

struct Command_Say {
  ChatChannel channel = ChatChannel::CHAT_CHANNEL_UNSPECIFIED;
  std::string text;
  std::string to;

  bool operator==(const Command_Say&) const = default;
};

struct Command_Open {
  Panel panel = Panel::PANEL_UNSPECIFIED;

  bool operator==(const Command_Open&) const = default;
};

struct Command_Close {
  Panel panel = Panel::PANEL_UNSPECIFIED;

  bool operator==(const Command_Close&) const = default;
};

struct Command_AhSearch {
  std::string query;

  bool operator==(const Command_AhSearch&) const = default;
};

struct Command_Who {
  std::string query;

  bool operator==(const Command_Who&) const = default;
};

struct Command_Act {
  enum class Verb : uint8_t {
    VERB_UNSPECIFIED = 0,
    TO = 1,
    AWAY = 2,
    TARGET = 3,
    USE = 4,
    STOP = 5,
    WAKE = 6,
    AVOID = 7,
    FACE = 8,
    FACE_AWAY = 9,
    JUMP = 10,
    CANCEL = 11,
    MARK = 12,
    INTERACT = 13,
    BOARD = 14,
    LEAVE = 15,
    EQUIP = 16,
    PET_ATTACK = 17,
    PET = 18,
    KITE = 19,
    OUT_OF = 20,
    BACK = 21,
  };
  using enum Verb;

  enum class PlaceKind : uint8_t {
    PLACE_UNSPECIFIED = 0,
    AT = 1,
    BEHIND = 2,
    IN_FRONT = 3,
    MAX_RANGE = 4,
    YD_FROM = 5,
  };
  using enum PlaceKind;

  enum class ThingKind : uint8_t {
    THING_UNSPECIFIED = 0,
    ME = 1,
    TANK = 2,
    OFFTANK = 3,
    HEALER = 4,
    LEADER = 5,
    ANYONE = 6,
    BOSS = 7,
    IT = 8,
    MY_TARGET = 9,
    FOCUS = 10,
    MARKER = 11,
    NEAREST_ALLY = 12,
    NEAREST_PLAYER = 13,
    NEAREST_ENEMY = 14,
    NEAREST_NAMED = 15,
    ALLY_WITH = 16,
    GROUP = 17,
    ALLIES = 18,
    NAMED = 19,
    // Where the party's last minimap ping is (`ext.minimap.ping`): a spot on
    // the ground, not a unit.
    PING = 20,
  };
  using enum ThingKind;

  enum class StopKind : uint8_t {
    STOP_UNSPECIFIED = 0,
    CASTING = 1,
    ATTACKING = 2,
    MOVING = 3,
  };
  using enum StopKind;

  enum class PetCmd : uint8_t {
    PET_CMD_UNSPECIFIED = 0,
    STAY = 1,
    FOLLOW = 2,
    PASSIVE = 3,
    DEFENSIVE = 4,
    DISMISS = 5,
  };
  using enum PetCmd;

  Command_Act::Verb verb = Command_Act::Verb::VERB_UNSPECIFIED;
  Command_Act::ThingKind thing = Command_Act::ThingKind::THING_UNSPECIFIED;
  Command_Act::PlaceKind place = Command_Act::PlaceKind::PLACE_UNSPECIFIED;
  Command_Act::StopKind stop = Command_Act::StopKind::STOP_UNSPECIFIED;
  Command_Act::PetCmd pet_cmd = Command_Act::PetCmd::PET_CMD_UNSPECIFIED;
  RaidMarker marker = RaidMarker::RAID_MARKER_UNSPECIFIED;
  std::string name;
  std::string target_name;
  float yd = 0.0f;
  std::string it_guid;
  std::string it_name;

  bool operator==(const Command_Act&) const = default;
};

struct Relative {
  enum class Height : uint8_t {
    HEIGHT_UNSPECIFIED = 0,
    LEVEL = 1,
    ABOVE = 2,
    BELOW = 3,
  };
  using enum Height;

  float dist_yd = 0.0f;
  Direction dir = Direction::DIRECTION_UNSPECIFIED;
  Relative::Height height = Relative::Height::HEIGHT_UNSPECIFIED;
  std::optional<bool> in_los;
  Vec3 position;
  bool behind_it = false;
  bool facing_me = false;

  bool operator==(const Relative&) const = default;
};

struct Spell {
  enum class School : uint8_t {
    SCHOOL_UNSPECIFIED = 0,
    PHYSICAL = 1,
    HOLY = 2,
    FIRE = 3,
    NATURE = 4,
    FROST = 5,
    SHADOW = 6,
    ARCANE = 7,
  };
  using enum School;

  using Cost = Spell_Cost;

  uint32_t id = 0;
  std::string name;
  uint32_t rank = 0;
  float cooldown_s = 0.0f;
  Spell::School school =
      Spell::School::SCHOOL_UNSPECIFIED;  // visibility: HOVER
  std::optional<Spell_Cost> cost;         // visibility: HOVER
  uint32_t cast_time_ms = 0;              // visibility: HOVER
  float range_yd = 0.0f;                  // visibility: HOVER
  std::optional<Tooltip> tooltip;         // visibility: HOVER
  std::optional<AbilityState> ability;
  std::optional<TrainerSpell> trainer;

  bool operator==(const Spell&) const = default;
};

struct Aura {
  uint32_t id = 0;
  std::string name;
  bool harmful = false;
  std::optional<float> remaining_s;
  uint32_t stacks = 0;
  DispelType dispel_type = DispelType::DISPEL_TYPE_UNSPECIFIED;
  bool dispellable_by_me = false;
  std::string caster;
  std::optional<Tooltip> tooltip;  // visibility: HOVER

  bool operator==(const Aura&) const = default;
};

struct Self_Indicators {
  using Durability = Self_Indicators_Durability;

  bool talent_points_unspent = false;
  std::vector<Self_Indicators_Durability> durability_warning;
  bool calendar_invite_pending = false;
  bool new_mail = false;
  // Unset when not known; 0 means the bags are full.
  std::optional<uint32_t> bag_free_slots;
  bool in_queue = false;
  bool achievement_toast = false;

  bool operator==(const Self_Indicators&) const = default;
};

struct Item {
  enum class Quality : uint8_t {
    QUALITY_UNSPECIFIED = 0,
    POOR = 1,
    COMMON = 2,
    UNCOMMON = 3,
    RARE_QUALITY = 4,
    EPIC = 5,
    LEGENDARY = 6,
    ARTIFACT = 7,
    HEIRLOOM = 8,
  };
  using enum Quality;

  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    EQUIPMENT = 1,
    CONSUMABLE = 2,
    QUEST = 3,
    TRADE_GOOD = 4,
    RECIPE = 5,
    CONTAINER = 6,
    REAGENT = 7,
    PROJECTILE = 8,
    JUNK = 9,
    KEY = 10,
    GEM = 11,
    GLYPH = 12,
    CURRENCY_TOKEN = 13,
    OTHER = 14,
  };
  using enum Kind;

  enum class Binding : uint8_t {
    BINDING_UNSPECIFIED = 0,
    NOT_BOUND = 1,
    SOULBOUND = 2,
    BIND_ON_PICKUP = 3,
    BIND_ON_EQUIP = 4,
    BIND_ON_USE = 5,
    BIND_TO_ACCOUNT = 6,
  };
  using enum Binding;

  uint32_t id = 0;
  std::string name;
  Item::Quality quality = Item::Quality::QUALITY_UNSPECIFIED;
  uint32_t count = 0;
  Item::Kind kind = Item::Kind::KIND_UNSPECIFIED;
  std::string bag_slot;
  bool usable = false;
  float cooldown_s = 0.0f;
  uint32_t charges = 0;
  bool starts_quest = false;
  uint32_t price = 0;
  uint32_t buyout = 0;
  std::string seller;
  std::string time_left;
  std::vector<CurrencyAmount> currency_price;
  uint32_t item_level = 0;      // visibility: HOVER
  uint32_t required_level = 0;  // visibility: HOVER
  Item::Binding binding =
      Item::Binding::BINDING_UNSPECIFIED;  // visibility: HOVER
  bool unique = false;                     // visibility: HOVER
  uint32_t vendor_value = 0;               // visibility: HOVER
  std::optional<Tooltip> tooltip;          // visibility: HOVER
  Box<EquipmentPart> equipment;

  bool operator==(const Item&) const = default;
};

struct Achievement {
  using Criterion = Achievement_Criterion;

  uint32_t id = 0;
  std::string name;
  std::string description;
  uint32_t points = 0;
  std::string category;
  bool earned = false;
  std::string earned_on;
  std::vector<Achievement_Criterion> criteria;
  std::string reward;

  bool operator==(const Achievement&) const = default;
};

struct Where {
  std::string map;
  std::string zone;
  std::string subzone;
  Vec3 position;
  float facing = 0.0f;
  bool indoors = false;
  bool in_instance = false;
  std::string pvp_status;

  bool operator==(const Where&) const = default;
};

struct Ui {
  std::vector<Panel> open_panels;
  std::vector<HoverRef> hovering;

  bool operator==(const Ui&) const = default;
};

struct World {
  using Queue = World_Queue;

  uint32_t session_minutes = 0;
  uint32_t daily_reset_in_s = 0;
  std::vector<std::string> server_messages;
  std::vector<WorldState> world_states;
  std::vector<World_Queue> queues;
  std::vector<std::string> holidays;

  bool operator==(const World&) const = default;
};

struct Map_Zone {
  std::string name;
  std::optional<LevelRange> level_range;

  bool operator==(const Map_Zone&) const = default;
};

struct Option {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    ATTACK = 1,
    TALK = 2,
    LOOT = 3,
    GATHER = 4,
    USE = 5,
    REST = 6,
    EXPLORE = 7,
    GO_TO = 8,
    KEEP = 9,
    INVITE = 10,
    ACCEPT_INVITE = 11,
    DECLINE_INVITE = 12,
    LEAVE_GROUP = 13,
  };
  using enum Kind;

  std::string text;
  std::vector<Blank> blanks;
  std::string guid;
  Option::Kind kind = Option::Kind::KIND_UNSPECIFIED;
  std::string name;

  bool operator==(const Option&) const = default;
};

struct Command_SetGoal {
  GoalPart::Kind goal_kind = GoalPart::Kind::KIND_UNSPECIFIED;
  std::vector<std::pair<std::string, std::string>> params;

  bool operator==(const Command_SetGoal&) const = default;
};

struct Command_Hover {
  std::optional<HoverRef> ref;

  bool operator==(const Command_Hover&) const = default;
};

struct Cast {
  std::optional<Spell> spell;
  uint32_t remaining_ms = 0;
  bool interruptible = false;
  std::string target;
  bool channeled = false;
  // Server DBC max effect radius in yards (0 for single-target/non-area casts).
  float radius_yd = 0.0f;
  // Impact world position (ground destination if ground-targeted, else caster
  // position when `radius_yd > 0`).
  std::optional<Vec3> impact_pos;

  bool operator==(const Cast&) const = default;
};

struct PetPart {
  enum class Happiness : uint8_t {
    HAPPINESS_UNSPECIFIED = 0,
    UNHAPPY = 1,
    CONTENT = 2,
    HAPPY = 3,
  };
  using enum Happiness;

  PetPart::Happiness happiness = PetPart::Happiness::HAPPINESS_UNSPECIFIED;
  std::vector<Spell> abilities;
  std::string stance;
  std::string owner;
  std::string family;

  bool operator==(const PetPart&) const = default;
};

struct VehiclePart_Aim {
  float pitch_deg = 0.0f;
  float min_pitch_deg = 0.0f;
  float max_pitch_deg = 0.0f;
  std::optional<Relative> impact;

  bool operator==(const VehiclePart_Aim&) const = default;
};

struct GameObject {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    QUEST_OBJECT = 1,
    HERB = 2,
    ORE = 3,
    CHEST = 4,
    MAILBOX = 5,
    DOOR = 6,
    CAMPFIRE = 7,
    BOBBER = 8,
    MEETING_STONE = 9,
    SUMMONING_PORTAL = 10,
    TRANSPORT = 11,
    FISHING_POOL = 12,
    BARBER_CHAIR = 13,
    GUILD_VAULT = 14,
    PORTAL = 15,
    RITUAL = 16,
    OTHER = 17,
    CAPTURE_POINT = 18,
    DESTRUCTIBLE = 19,
  };
  using enum Kind;

  std::string guid;
  std::string name;
  Relative rel;
  GameObject::Kind kind = GameObject::Kind::KIND_UNSPECIFIED;
  bool usable = false;
  std::string state;
  bool highlighted = false;
  uint32_t required_skill = 0;
  std::optional<float> health_pct;
  std::string owner;
  float capture_progress_pct = 0.0f;

  bool operator==(const GameObject&) const = default;
};

struct Area {
  std::string guid;
  std::string name;
  std::optional<Relative> rel;
  std::optional<Spell> spell;
  float radius_yd = 0.0f;
  bool inside_me = false;
  bool hostile = false;
  std::optional<float> remaining_s;
  // Seconds until a ground-targeted cast lands at this area's spot (unset for
  // already-landed ground effects).
  std::optional<float> lands_in_s;

  bool operator==(const Area&) const = default;
};

struct Place {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    TOWN = 1,
    CAMP = 2,
    FLIGHT_MASTER = 3,
    INNKEEPER = 4,
    GRAVEYARD = 5,
    DUNGEON = 6,
    RAID = 7,
    ZONE_EXIT = 8,
    LANDMARK = 9,
    CAMPFIRE = 10,
    AUCTION_HOUSE = 11,
    BANK = 12,
    TRAINER = 13,
    VENDOR = 14,
    DOCK = 15,
    ZEPPELIN_TOWER = 16,
    ZONE = 17,
    OBJECTIVE_AREA = 18,
    BATTLEMASTER = 19,
    GUILD_BANK = 20,
    BARBER_SHOP = 21,
    STABLE = 22,
  };
  using enum Kind;

  std::string name;
  Relative rel;
  Place::Kind kind = Place::Kind::KIND_UNSPECIFIED;
  bool known = false;
  std::optional<LevelRange> level_range;  // visibility: HOVER
  std::string quest;

  bool operator==(const Place&) const = default;
};

struct EquipmentPart_Socket {
  std::string color;
  std::optional<Item> gem;

  bool operator==(const EquipmentPart_Socket&) const = default;
};

struct QuestPart {
  uint32_t quest_id = 0;
  std::string quest;
  std::string text;
  bool complete = false;
  bool failed = false;
  bool daily = false;
  float timer_s = 0.0f;
  std::string escort;
  std::string tag;
  uint32_t level = 0;
  std::optional<Spell> item_button;
  // Party/raid members who also have this quest in their quest log (F3);
  // `party_members` is capped for raid snapshots while `party_member_count`
  // carries the true unclipped total (0 falls back to `party_members.size()`).
  std::vector<std::string> party_members;
  uint32_t party_member_count = 0;

  bool operator==(const QuestPart&) const = default;
};

struct ActionBars {
  std::vector<Spell> buttons;
  std::vector<Spell> stance_bar;
  std::vector<Spell> totem_bar;

  bool operator==(const ActionBars&) const = default;
};

struct Interaction_QuestDialog {
  std::string title;
  std::string text;
  std::string objectives;
  std::vector<Item> rewards;
  std::vector<Item> choices;
  uint32_t money = 0;
  uint32_t xp = 0;
  std::string stage;

  bool operator==(const Interaction_QuestDialog&) const = default;
};

struct Bags {
  using Hearthstone = Bags_Hearthstone;

  std::vector<Item> items;
  uint32_t free_slots = 0;
  uint32_t total_slots = 0;
  std::optional<Bags_Hearthstone> hearthstone;
  std::vector<Item> keyring;

  bool operator==(const Bags&) const = default;
};

struct Character {
  using EquipmentSet = Character_EquipmentSet;

  std::vector<Item> equipped;
  uint32_t gearscore = 0;  // addon: GearScore
  std::vector<StatValue> stats;
  std::vector<std::string> titles;
  std::string active_title;
  std::vector<Character_EquipmentSet> equipment_sets;
  std::vector<CurrencyAmount> currency;

  bool operator==(const Character&) const = default;
};

struct Spellbook {
  std::vector<Spell> spells;
  std::vector<Spell> pet_spells;

  bool operator==(const Spellbook&) const = default;
};

struct PromptEvent {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    GROUP_INVITE = 1,
    TRADE = 2,
    DUEL = 3,
    GUILD_INVITE = 4,
    READY_CHECK = 5,
    ROLE_CHECK = 6,
    RESURRECT_OFFER = 7,
    SUMMON = 8,
    QUEST_SHARE = 9,
    BATTLEGROUND_READY = 10,
    ARENA_TEAM_INVITE = 11,
    WINTERGRASP_INVITE = 12,
    BIND_HEARTHSTONE = 13,
    CONFIRM_BIND_ON_PICKUP = 14,
    CONFIRM_DELETE_ITEM = 15,
    CONFIRM_ABANDON_QUEST = 16,
    CONFIRM_REPLACE_ENCHANT = 17,
    CONFIRM_TALENT_WIPE = 18,
    CONFIRM_RELEASE_SPIRIT = 19,
    CONFIRM_LOOT_ROLL = 20,
    CONFIRM_PETITION_SIGN = 21,
    CONFIRM_LEAVE_BATTLEGROUND = 22,
    CONFIRM_INSTANCE_LOCK = 23,
    CONFIRM_SPIRIT_HEALER = 24,
    CONFIRM_GUILD_LEADER = 25,
    CONFIRM_REPLACE_GEM = 26,
    LFG_PROPOSAL = 27,
    LFG_BOOT_VOTE = 28,
    LFG_OFFER_CONTINUE = 29,
    CONFIRM_PURCHASE = 30,
    SPIRIT_HEALER_WAVE = 31,
    RECLAIM_CORPSE = 32,
    CHANNEL_INVITE = 33,
    CHANNEL_PASSWORD = 34,
    PET_NAME = 35,
  };
  using enum Kind;

  PromptEvent::Kind kind = PromptEvent::Kind::KIND_UNSPECIFIED;
  std::string from;
  std::string text;
  float timeout_s = 0.0f;
  std::optional<Item> item;
  uint32_t cost = 0;
  std::string dungeon;
  Role role = Role::ROLE_UNSPECIFIED;
  std::string target;
  std::string reason;

  bool operator==(const PromptEvent&) const = default;
};

struct ItemReceivedEvent {
  std::optional<Item> item;
  std::string how;

  bool operator==(const ItemReceivedEvent&) const = default;
};

struct LootRollEvent {
  std::optional<Item> item;
  uint32_t roll_id = 0;

  bool operator==(const LootRollEvent&) const = default;
};

struct LfgEvent {
  std::string what;
  std::string dungeon;
  std::vector<Item> rewards;
  uint32_t money = 0;
  uint32_t xp = 0;

  bool operator==(const LfgEvent&) const = default;
};

// The socket protocol's version: every ModuleMessage and Command carries it,
// and each side drops frames of another version (a module and a harness from
// different checkouts). Adding a field doesn't change it (an older reader
// skips unknown keys); changing or removing what a field means does.
// 2: standing-order acts finish through `done_act_command_id`, no longer
// `done_command_id`.
// 3: added group Option::Kind values (INVITE, ACCEPT_INVITE, DECLINE_INVITE,
// LEAVE_GROUP) and General/Trade/Party chat channels.
inline constexpr uint32_t kWireVersion = 4;

// What one build speaks: sent once each way when a connection opens, so two
// builds that can't talk refuse at once with one clear message instead of
// dropping or misreading frames. A side whose hello differs from its own
// (another kWireVersion or SchemaHash()) refuses the connection; a client that
// sends no hello (a passive capture tool) is still served.
struct Hello {
  uint32_t wire_version = 0;
  // SchemaHash() of the sender's build.
  std::string schema_hash;
  // Who sent it: "module", "harness", or a tool's name (logs only).
  std::string role;

  bool operator==(const Hello&) const = default;
};

struct Command {
  using Choose = Command_Choose;
  using SetGoal = Command_SetGoal;
  using ClearGoal = Command_ClearGoal;
  using Say = Command_Say;
  using Open = Command_Open;
  using Close = Command_Close;
  using Hover = Command_Hover;
  using AhSearch = Command_AhSearch;
  using Who = Command_Who;
  using Act = Command_Act;

  // kWireVersion of the sender (set by the harness when it sends).
  uint32_t version = 0;
  std::string bot;
  uint32_t command_id = 0;
  std::optional<Command_Choose> choose;
  std::optional<Command_SetGoal> set_goal;
  std::optional<Command_ClearGoal> clear_goal;
  std::optional<Command_Say> say;
  std::optional<Command_Open> open;
  std::optional<Command_Close> close;
  std::optional<Command_Hover> hover;
  std::optional<Command_AhSearch> ah_search;
  std::optional<Command_Who> who;
  std::optional<Command_Act> act;
  // The client's hello: its first frame (no `bot`), optional.
  std::optional<Hello> hello;

  bool operator==(const Command&) const = default;
};

struct Activity {
  enum class State : uint8_t {
    STATE_UNSPECIFIED = 0,
    IDLE = 1,
    MOVING = 2,
    PATROLLING = 3,
    APPROACHING_ME = 4,
    FLEEING = 5,
    FIGHTING = 6,
    CASTING = 7,
    CHANNELING = 8,
    TALKING = 9,
    EMOTING = 10,
    LOOTING = 11,
    GATHERING = 12,
    SITTING = 13,
    FOLLOWING = 14,
    FISHING = 15,
    DEAD = 16,
    DRINKING = 17,
    STEALTHED = 18,
    AFK = 19,
  };
  using enum State;

  Activity::State state = Activity::State::STATE_UNSPECIFIED;
  std::optional<Cast> cast;
  std::string with;
  std::string text;

  bool operator==(const Activity&) const = default;
};

struct VehiclePart {
  using Seat = VehiclePart_Seat;
  using Aim = VehiclePart_Aim;

  std::vector<VehiclePart_Seat> seats;
  std::vector<Spell> abilities;
  std::optional<VehiclePart_Aim> aim;

  bool operator==(const VehiclePart&) const = default;
};

struct EquipmentPart {
  enum class Slot : uint8_t {
    SLOT_UNSPECIFIED = 0,
    HEAD = 1,
    NECK = 2,
    SHOULDER = 3,
    BACK = 4,
    CHEST = 5,
    SHIRT = 6,
    TABARD = 7,
    WRIST = 8,
    HANDS = 9,
    WAIST = 10,
    LEGS = 11,
    FEET = 12,
    FINGER1 = 13,
    FINGER2 = 14,
    TRINKET1 = 15,
    TRINKET2 = 16,
    MAIN_HAND = 17,
    OFF_HAND = 18,
    RANGED = 19,
    BAG = 20,
  };
  using enum Slot;

  using Socket = EquipmentPart_Socket;

  EquipmentPart::Slot slot = EquipmentPart::Slot::SLOT_UNSPECIFIED;
  float durability_pct = 0.0f;
  std::vector<StatValue> stats;               // visibility: HOVER
  std::vector<EquipmentPart_Socket> sockets;  // visibility: HOVER
  std::string socket_bonus;                   // visibility: HOVER
  std::string enchant;                        // visibility: HOVER
  std::string type;                           // visibility: HOVER
  uint32_t gearscore = 0;       // visibility: HOVER, addon: GearScore
  int32_t gearscore_delta = 0;  // visibility: HOVER, addon: GearScore
  bool upgrade = false;         // visibility: HOVER, addon: GearScore

  bool operator==(const EquipmentPart&) const = default;
};

struct Objective {
  using Progress = Objective_Progress;

  std::string title;
  std::optional<Objective_Progress> progress;
  bool done = false;
  std::optional<Place> where;
  std::optional<QuestPart> quest;
  std::optional<GoalPart> goal;
  std::optional<AchievementPart> achievement;

  bool operator==(const Objective&) const = default;
};

struct Instance {
  using Boss = Instance_Boss;

  std::string name;
  std::string difficulty;
  uint32_t size = 0;
  std::vector<Instance_Boss> bosses;
  std::string locked_until;
  std::optional<Place> entrance;

  bool operator==(const Instance&) const = default;
};

struct Death {
  std::optional<Relative> corpse;
  float can_resurrect_in_s = 0.0f;
  std::optional<Place> spirit_healer;
  std::string resurrect_offer_from;
  std::string self_resurrect;

  bool operator==(const Death&) const = default;
};

struct Map_Questie {
  std::vector<Place> quest_givers;
  std::vector<Place> spawn_areas;

  bool operator==(const Map_Questie&) const = default;
};

struct Map_InstanceMap {
  std::string floor;
  std::vector<Place> areas;

  bool operator==(const Map_InstanceMap&) const = default;
};

struct ChatLink {
  std::string text;
  std::optional<Item> item;
  std::optional<Spell> spell;
  std::optional<QuestPart> quest;
  std::optional<Achievement> achievement;

  bool operator==(const ChatLink&) const = default;
};

struct PathFailedEvent {
  std::optional<Place> to;

  bool operator==(const PathFailedEvent&) const = default;
};

struct Unit {
  enum class Kind : uint8_t {
    KIND_UNSPECIFIED = 0,
    NPC = 1,
    PLAYER = 2,
    PET = 3,
    VEHICLE = 4,
    TOTEM = 5,
    CRITTER = 6,
  };
  using enum Kind;

  enum class LevelColor : uint8_t {
    LEVEL_COLOR_UNSPECIFIED = 0,
    GRAY = 1,
    GREEN = 2,
    YELLOW = 3,
    ORANGE = 4,
    RED = 5,
    SKULL_LEVEL = 6,
  };
  using enum LevelColor;

  enum class Reaction : uint8_t {
    REACTION_UNSPECIFIED = 0,
    HOSTILE = 1,
    NEUTRAL = 2,
    FRIENDLY = 3,
  };
  using enum Reaction;

  enum class Classification : uint8_t {
    CLASSIFICATION_UNSPECIFIED = 0,
    NORMAL = 1,
    ELITE = 2,
    RARE = 3,
    RARE_ELITE = 4,
    WORLD_BOSS = 5,
  };
  using enum Classification;

  enum class ThreatStatus : uint8_t {
    THREAT_STATUS_UNSPECIFIED = 0,
    THREAT_NONE = 1,
    THREAT_HIGH = 2,
    THREAT_AGGRO = 3,
  };
  using enum ThreatStatus;

  enum class QuestMarker : uint8_t {
    QUEST_MARKER_UNSPECIFIED = 0,
    QUEST_AVAILABLE = 1,
    QUEST_AVAILABLE_LOW = 2,
    QUEST_TURN_IN = 3,
    QUEST_INCOMPLETE = 4,
    QUEST_DAILY = 5,
  };
  using enum QuestMarker;

  enum class Service : uint8_t {
    SERVICE_UNSPECIFIED = 0,
    VENDOR = 1,
    CLASS_TRAINER = 2,
    PROFESSION_TRAINER = 3,
    REPAIR = 4,
    INNKEEPER = 5,
    FLIGHT_MASTER = 6,
    BANKER = 7,
    AUCTIONEER = 8,
    STABLE_MASTER = 9,
    SPIRIT_HEALER = 10,
    BATTLEMASTER = 11,
    GUILD_MASTER = 12,
    TABARD_VENDOR = 13,
    QUEST_GIVER = 14,
    GUILD_BANKER = 15,
    BARBER = 16,
    ARENA_ORGANIZER = 17,
    WEAPON_MASTER = 18,
  };
  using enum Service;

  enum class VisualState : uint8_t {
    VISUAL_STATE_UNSPECIFIED = 0,
    POLYMORPHED = 1,
    STUNNED = 2,
    FEARED = 3,
    SAPPED = 4,
    ASLEEP = 5,
    BANISHED = 6,
    FROZEN = 7,
    ICE_BLOCK = 8,
    BUBBLED = 9,
    MIND_CONTROLLED = 10,
    STEALTH_SHIMMER = 11,
    SHACKLED = 12,
    ROOTED = 13,
    DRUNK_SWAY = 14,
    FLYING = 15,
    UNTARGETABLE = 16,
    SUBMERGED = 17,
  };
  using enum VisualState;

  enum class Size : uint8_t {
    SIZE_UNSPECIFIED = 0,
    SMALL = 1,
    MEDIUM = 2,
    LARGE = 3,
    HUGE = 4,
  };
  using enum Size;

  enum class Detail : uint8_t {
    DETAIL_UNSPECIFIED = 0,
    NAMEPLATE = 1,
    MODEL = 2,
  };
  using enum Detail;

  using Pack = Unit_Pack;

  std::string guid;
  std::string name;
  Relative rel;
  Unit::Kind kind = Unit::Kind::KIND_UNSPECIFIED;
  std::optional<uint32_t> level;
  Unit::LevelColor level_color = Unit::LevelColor::LEVEL_COLOR_UNSPECIFIED;
  Unit::Reaction reaction = Unit::Reaction::REACTION_UNSPECIFIED;
  Unit::Classification classification =
      Unit::Classification::CLASSIFICATION_UNSPECIFIED;
  std::optional<float> health_pct;
  std::optional<float> power_pct;
  bool dead = false;
  bool lootable = false;
  bool skinnable = false;
  bool tapped_by_other = false;
  bool targeting_me = false;
  std::string target;
  Unit::ThreatStatus threat_status =
      Unit::ThreatStatus::THREAT_STATUS_UNSPECIFIED;
  Unit::QuestMarker quest_marker = Unit::QuestMarker::QUEST_MARKER_UNSPECIFIED;
  bool pvp_flagged = false;
  Activity activity;
  std::vector<Aura> auras;
  RaidMarker raid_marker = RaidMarker::RAID_MARKER_UNSPECIFIED;
  std::optional<Unit_Pack> pack;
  std::vector<Unit::VisualState> visual_state;
  // A loss-of-control aura (confuse, fear, stun, transform) that any damage
  // breaks, by its Spell.dbc aura interrupt flags: Polymorph, Sap, a trap.
  std::optional<Aura> crowd_control;
  // Who the mob is locked onto whatever the threat (a scripted fixate, or a
  // taunt aura's caster): threat can't pull it off them.
  std::string fixated_on;
  // Immune to taunt auras and 'attack me' effects (the creature's spell
  // immunity state, as a taunt's "Immune" shows).
  bool taunt_immune = false;
  std::string quest_relevant;           // addon: Questie
  std::vector<Unit::Service> services;  // visibility: HOVER
  std::string subtitle;
  std::string looks_like;
  Unit::Size size = Unit::Size::SIZE_UNSPECIFIED;
  Unit::Detail detail = Unit::Detail::DETAIL_UNSPECIFIED;
  std::optional<PlayerPart> player;
  std::optional<GroupPart> group;
  std::optional<BossPart> boss;
  std::optional<PetPart> pet;
  std::optional<VehiclePart> vehicle;

  bool operator==(const Unit&) const = default;
};

struct Map {
  using Zone = Map_Zone;
  using Questie = Map_Questie;
  using InstanceMap = Map_InstanceMap;

  std::optional<Map_Zone> zone;
  std::vector<Place> places;
  std::vector<Place> bordering_zones;
  std::vector<Place> objective_locations;
  std::optional<Map_Questie> questie;  // addon: Questie
  std::optional<Map_InstanceMap> instance_map;

  bool operator==(const Map&) const = default;
};

struct QuestLog_Entry {
  std::optional<Objective> objective;
  std::string description;
  std::vector<Item> rewards;
  std::vector<Item> choices;
  uint32_t money = 0;
  bool tracked = false;
  bool shareable = false;

  bool operator==(const QuestLog_Entry&) const = default;
};

struct ChatEvent {
  ChatChannel channel = ChatChannel::CHAT_CHANNEL_UNSPECIFIED;
  std::string channel_name;
  std::string from;
  std::string text;
  bool at_me = false;
  float from_dist_yd = 0.0f;
  std::vector<ChatLink> links;

  bool operator==(const ChatEvent&) const = default;
};

struct Self {
  enum class Gender : uint8_t {
    GENDER_UNSPECIFIED = 0,
    MALE = 1,
    FEMALE = 2,
  };
  using enum Gender;

  enum class PowerType : uint8_t {
    POWER_TYPE_UNSPECIFIED = 0,
    MANA = 1,
    RAGE = 2,
    ENERGY = 3,
    RUNIC_POWER = 4,
    FOCUS = 5,
  };
  using enum PowerType;

  enum class LifeState : uint8_t {
    LIFE_STATE_UNSPECIFIED = 0,
    ALIVE = 1,
    DEAD_BODY = 2,
    GHOST = 3,
  };
  using enum LifeState;

  enum class Drunk : uint8_t {
    DRUNK_UNSPECIFIED = 0,
    SOBER = 1,
    TIPSY = 2,
    DRUNK = 3,
    SMASHED = 4,
  };
  using enum Drunk;

  using Control = Self_Control;
  using Indicators = Self_Indicators;
  using AutoAttack = Self_AutoAttack;
  using WeaponEnchant = Self_WeaponEnchant;
  using Rune = Self_Rune;

  Unit unit;
  Self::Gender gender = Self::Gender::GENDER_UNSPECIFIED;
  uint32_t xp = 0;
  uint32_t xp_next = 0;
  uint32_t rested_xp = 0;
  uint32_t health = 0;
  uint32_t health_max = 0;
  Self::PowerType power_type = Self::PowerType::POWER_TYPE_UNSPECIFIED;
  uint32_t power = 0;
  uint32_t power_max = 0;
  uint32_t alt_mana = 0;
  uint32_t alt_mana_max = 0;
  Self::LifeState state = Self::LifeState::LIFE_STATE_UNSPECIFIED;
  bool in_combat = false;
  bool resting = false;
  bool camping = false;
  bool mounted = false;
  bool swimming = false;
  // Liquid type the bot is currently in ("water", "lava", "slime"), or "".
  std::string liquid;
  bool in_flight = false;
  bool on_transport = false;
  Self_Control control;
  std::optional<Cast> casting;
  uint32_t money = 0;
  Self_Indicators indicators;
  uint32_t gcd_remaining_ms = 0;
  std::optional<Self_AutoAttack> auto_attack;
  bool in_cinematic = false;
  Self::Drunk drunk = Self::Drunk::DRUNK_UNSPECIFIED;
  std::vector<Self_WeaponEnchant> weapon_enchants;
  std::vector<Self_Rune> runes;
  std::string form;
  bool pvp_flagged = false;
  bool group_leader = false;

  bool operator==(const Self&) const = default;
};

struct Nearby {
  std::vector<Unit> units;
  std::vector<GameObject> objects;
  std::vector<Area> areas;

  bool operator==(const Nearby&) const = default;
};

struct Tooltipped {
  std::optional<HoverRef> ref;
  std::optional<Tooltip> tooltip;
  std::optional<Item> item;
  std::optional<Spell> spell;
  std::optional<Aura> aura;
  std::optional<Unit> unit;
  std::optional<Place> place;
  std::optional<Achievement> achievement;
  std::optional<GameObject> game_object;
  std::string questie;  // addon: Questie
  std::vector<Item> compare;

  bool operator==(const Tooltipped&) const = default;
};

// The last minimap ping a party member made (MSG_MINIMAP_PING), as the
// client flashes it on the minimap: who pinged, where on the ground (the ping
// itself is 2D; `rel.position.z` is the ground there), and how long ago.
struct Minimap_Ping {
  std::string from;
  Relative rel;
  float age_s = 0.0f;

  bool operator==(const Minimap_Ping&) const = default;
};

struct Minimap {
  using Ping = Minimap_Ping;
  std::string zone;
  std::string subzone;
  std::string clock;
  std::vector<Place> places;
  std::vector<Place> objective_areas;
  std::vector<Unit> tracked;
  std::vector<GameObject> tracked_objects;
  std::vector<Unit> party;
  std::optional<Relative> corpse;
  std::string tracking;
  std::optional<Minimap_Ping> ping;

  bool operator==(const Minimap&) const = default;
};

struct Target {
  std::optional<Unit> target;
  std::optional<Unit> focus;
  std::string target_of_target;
  float my_threat_pct = 0.0f;

  bool operator==(const Target&) const = default;
};

struct Combat {
  using Threat = Combat_Threat;
  using Resources = Combat_Resources;

  std::vector<Unit> attackers;
  std::vector<Combat_Threat> threat;  // addon: Omen
  std::optional<Combat_Resources> resources;
  std::string summary;
  std::string duel_opponent;

  bool operator==(const Combat&) const = default;
};

struct Party {
  std::string leader;
  std::vector<Unit> members;
  std::vector<std::string> missing_buffs_i_can_give;
  std::string loot_method;
  std::string loot_threshold;
  std::string difficulty;
  bool lfg = false;

  bool operator==(const Party&) const = default;
};

struct Raid {
  std::vector<Unit> members;  // addon: Grid
  std::vector<std::string> main_tanks;
  std::vector<std::string> main_assists;

  bool operator==(const Raid&) const = default;
};

struct Encounter {
  using Timer = Encounter_Timer;

  std::optional<Unit> boss;
  std::vector<Encounter_Timer> dbm;  // addon: DBM

  bool operator==(const Encounter&) const = default;
};

struct PetFrame {
  using Totem = PetFrame_Totem;

  std::optional<Unit> pet;
  std::vector<PetFrame_Totem> totems;

  bool operator==(const PetFrame&) const = default;
};

struct QuestLog {
  using Entry = QuestLog_Entry;

  std::vector<QuestLog_Entry> quests;

  bool operator==(const QuestLog&) const = default;
};

struct Event {
  std::optional<ChatEvent> chat;
  std::optional<ErrorEvent> error;
  std::optional<InfoEvent> info;
  std::optional<PromptEvent> prompt;
  std::optional<LevelUpEvent> level_up;
  std::optional<XpGainedEvent> xp_gained;
  std::optional<MoneyChangedEvent> money_changed;
  std::optional<ItemReceivedEvent> item_received;
  std::optional<QuestEvent> quest_complete;
  std::optional<QuestEvent> quest_failed;
  std::optional<QuestEvent> quest_accepted;
  std::optional<DiedEvent> died;
  std::optional<ResurrectedEvent> resurrected;
  std::optional<LootRollEvent> loot_roll;
  std::optional<PathFailedEvent> path_failed;
  std::optional<DuelEvent> duel;
  std::optional<KickedEvent> kicked;
  std::optional<DbmWarningEvent> dbm_warning;  // addon: DBM
  std::optional<LfgEvent> lfg;
  std::optional<DisplacedEvent> displaced;

  bool operator==(const Event&) const = default;
};

struct Base {
  Self self;
  Where where;
  std::optional<Doing> doing;
  Nearby nearby;
  Ui ui;
  std::vector<Tooltipped> tooltips;  // visibility: HOVER

  bool operator==(const Base&) const = default;
};

struct Interaction {
  using QuestDialog = Interaction_QuestDialog;

  std::string with;
  std::optional<Interaction_QuestDialog> quest;

  bool operator==(const Interaction&) const = default;
};

// The trade window while it is open (SMSG_TRADE_STATUS /
// SMSG_TRADE_STATUS_EXTENDED, as the client shows it): who the bot trades
// with, the items and money each side has put up for trade, and whose
// Trade button is pressed. Changing either side's offer releases both.
struct Trade {
  std::string with;
  std::vector<Item> giving;  // The bot's side (traded slots only).
  std::vector<Item> getting;
  uint32_t giving_money = 0;  // Copper.
  uint32_t getting_money = 0;
  bool i_accepted = false;
  bool they_accepted = false;

  bool operator==(const Trade&) const = default;
};

struct ImmediateEvent {
  std::string bot;
  uint32_t seq = 0;
  int64_t time_ms = 0;
  Event event;
  int64_t monotonic_us = 0;

  bool operator==(const ImmediateEvent&) const = default;
};

struct Extensions {
  std::optional<Minimap> minimap;
  std::vector<Objective> quest_tracker;
  std::optional<ActionBars> action_bars;
  std::optional<World> world;
  std::optional<Target> target;
  std::optional<Combat> combat;
  std::optional<Party> party;
  std::optional<Raid> raid;
  std::optional<Encounter> encounter;
  std::optional<Instance> instance;
  std::optional<Death> death;
  std::optional<Unit> vehicle;
  std::optional<Interaction> interaction;
  std::optional<Trade> trade;
  std::optional<PetFrame> pet;
  std::optional<Unit> possessed;
  std::optional<Bags> bags;            // visibility: OPEN
  std::optional<Map> map;              // visibility: OPEN
  std::optional<Character> character;  // visibility: OPEN
  std::optional<QuestLog> quest_log;   // visibility: OPEN
  std::optional<Spellbook> spellbook;  // visibility: OPEN

  bool operator==(const Extensions&) const = default;
};

struct Snapshot {
  std::string bot;
  uint32_t seq = 0;
  int64_t time_ms = 0;
  int64_t monotonic_us = 0;
  // The `command_id` of the last Choose the body finished carrying out (the
  // target died, the loot window closed, the object was clicked); 0 before
  // any. Repeated on every snapshot until the next one finishes, so a newer
  // snapshot can't lose it (docs/specs/snapshot.md, "Commands").
  uint32_t done_command_id = 0;
  // Same for the last standing-order `Command::Act` finished by the body;
  // kept separate so an option completion never overwrites an act completion.
  uint32_t done_act_command_id = 0;
  Base base;
  Extensions ext;
  std::vector<Event> events;
  std::vector<Option> options;

  bool operator==(const Snapshot&) const = default;
};

struct ModuleMessage {
  // kWireVersion of the sender (set by the module when it sends).
  uint32_t version = 0;
  std::optional<Snapshot> snapshot;
  std::optional<ImmediateEvent> event;
  // The module's hello: the first frame of every connection, before any
  // snapshot or event.
  std::optional<Hello> hello;

  bool operator==(const ModuleMessage&) const = default;
};

struct OptionFacts_Npc {
  std::string guid;
  uint32_t npc_flags = 0;

  bool operator==(const OptionFacts_Npc&) const = default;
};

struct OptionFacts_Spot {
  std::string id;
  Direction dir = Direction::DIRECTION_UNSPECIFIED;
  float dist_yd = 0.0f;
  std::string label;
  Vec3 pos;

  bool operator==(const OptionFacts_Spot&) const = default;
};

struct OptionFacts_Place {
  std::string id;
  std::string name;
  std::string services;
  Direction dir = Direction::DIRECTION_UNSPECIFIED;
  float dist_yd = 0.0f;
  Vec3 pos;

  bool operator==(const OptionFacts_Place&) const = default;
};

struct OptionFacts_Item {
  uint32_t entry = 0;
  std::string name;
  bool out_of_combat_only = false;
  bool equippable = false;
  std::string detail;

  bool operator==(const OptionFacts_Item&) const = default;
};

struct OptionFacts_PartyTarget {
  std::string member_name;
  std::string target_guid;

  bool operator==(const OptionFacts_PartyTarget&) const = default;
};

struct OptionFacts_TalentTree {
  uint32_t tab = 0;
  std::string name;
  uint32_t spent = 0;
  uint32_t free_points = 0;

  bool operator==(const OptionFacts_TalentTree&) const = default;
};

struct OptionFacts_NpcAction {
  std::string action_guid;
  std::string npc_guid;
  std::string label;

  bool operator==(const OptionFacts_NpcAction&) const = default;
};

struct OptionFacts_Action {
  std::string guid;
  std::string label;

  bool operator==(const OptionFacts_Action&) const = default;
};

// Game data a player knows (or sees on hover) that the glance snapshot does
// not carry, read by llm_brain::AddOptions both live and in fixture_options
// (<case>.facts.json).
struct OptionFacts {
  using Npc = OptionFacts_Npc;
  using Spot = OptionFacts_Spot;
  using Place = OptionFacts_Place;
  using Item = OptionFacts_Item;
  using PartyTarget = OptionFacts_PartyTarget;
  using TalentTree = OptionFacts_TalentTree;
  using NpcAction = OptionFacts_NpcAction;
  using Action = OptionFacts_Action;

  std::vector<OptionFacts_Npc> npcs;
  bool has_food = false;
  bool has_drink = false;
  uint32_t herbalism_skill = 0;
  uint32_t mining_skill = 0;
  std::vector<OptionFacts_Spot> spots;
  std::vector<OptionFacts_Place> places;
  std::vector<OptionFacts_Item> usable_items;
  std::vector<OptionFacts_PartyTarget> party_targets;
  std::vector<OptionFacts_TalentTree> talent_trees;
  std::vector<OptionFacts_NpcAction> npc_actions;
  std::vector<OptionFacts_Action> group_and_upkeep_actions;
  std::string pending_group_invite_from;
  std::vector<std::string> invite_candidates;
  bool can_leave_group = false;
  bool has_pending_resurrect = false;
  // The self-resurrection the dead bot can use now (Soulstone Resurrection,
  // Reincarnation), by name; empty when none.
  std::string self_resurrect;
  // No Attack on a mob nobody in the party is fighting yet: a human leads
  // and this bot isn't the group's tank (or the leader tanks), or a human in
  // the group tanks, is AFK or is over 25 yd away.
  bool hold_pulls = false;
  // No Explore: the bot leads a dungeon group while a human in it tanks, is
  // AFK or is over 25 yd away.
  bool hold_explore = false;

  bool operator==(const OptionFacts&) const = default;
};

template <typename T>
Box<T>::~Box() = default;

template <typename T>
Box<T>::Box(const Box& other)
    : ptr_(other.ptr_ ? std::make_unique<T>(*other.ptr_) : nullptr) {}

template <typename T>
Box<T>& Box<T>::operator=(const Box& other) {
  if (this != &other) {
    ptr_ = other.ptr_ ? std::make_unique<T>(*other.ptr_) : nullptr;
  }
  return *this;
}

template <typename T>
bool Box<T>::operator==(const Box& other) const {
  if (!ptr_ && !other.ptr_) {
    return true;
  }
  if (!ptr_ || !other.ptr_) {
    return false;
  }
  return *ptr_ == *other.ptr_;
}

// Enum enumerator name (e.g. "PLAYER", "NE", "MANA"), or "" if unknown.
std::string_view EnumName(Direction value);
std::string_view EnumName(DispelType value);
std::string_view EnumName(RaidMarker value);
std::string_view EnumName(Role value);
std::string_view EnumName(Stat value);
std::string_view EnumName(Panel value);
std::string_view EnumName(ChatChannel value);
std::string_view EnumName(Self_Indicators_Durability::Level value);
std::string_view EnumName(GoalPart::Kind value);
std::string_view EnumName(HoverRef::Kind value);
std::string_view EnumName(DisplacedEvent::How value);
std::string_view EnumName(Blank::Kind value);
std::string_view EnumName(Option::Kind value);
std::string_view EnumName(Command_Act::Verb value);
std::string_view EnumName(Command_Act::PlaceKind value);
std::string_view EnumName(Command_Act::ThingKind value);
std::string_view EnumName(Command_Act::StopKind value);
std::string_view EnumName(Command_Act::PetCmd value);
std::string_view EnumName(Relative::Height value);
std::string_view EnumName(Spell::School value);
std::string_view EnumName(Item::Quality value);
std::string_view EnumName(Item::Kind value);
std::string_view EnumName(Item::Binding value);
std::string_view EnumName(PetPart::Happiness value);
std::string_view EnumName(GameObject::Kind value);
std::string_view EnumName(Place::Kind value);
std::string_view EnumName(PromptEvent::Kind value);
std::string_view EnumName(Activity::State value);
std::string_view EnumName(EquipmentPart::Slot value);
std::string_view EnumName(Unit::Kind value);
std::string_view EnumName(Unit::LevelColor value);
std::string_view EnumName(Unit::Reaction value);
std::string_view EnumName(Unit::Classification value);
std::string_view EnumName(Unit::ThreatStatus value);
std::string_view EnumName(Unit::QuestMarker value);
std::string_view EnumName(Unit::Service value);
std::string_view EnumName(Unit::VisualState value);
std::string_view EnumName(Unit::Size value);
std::string_view EnumName(Unit::Detail value);
std::string_view EnumName(Self::Gender value);
std::string_view EnumName(Self::PowerType value);
std::string_view EnumName(Self::LifeState value);
std::string_view EnumName(Self::Drunk value);

// Binary BEVE wire codec (module <-> harness over Unix socket).
std::string WriteBeve(const ModuleMessage& msg);
bool ReadBeve(std::string_view bytes, ModuleMessage* out);

std::string WriteBeve(const Command& cmd);
bool ReadBeve(std::string_view bytes, Command* out);

std::string WriteBeve(const Snapshot& snap);
bool ReadBeve(std::string_view bytes, Snapshot* out);

// JSON codec (spec examples, eval/screen fixtures, debug tools). WriteJson
// returns "" when the value can't be written (an enum value with no name).
std::string WriteJson(const Snapshot& snap, bool pretty = false);
bool ReadJson(std::string_view json,
              Snapshot* out,
              std::string* error = nullptr);

std::string WriteJson(const ModuleMessage& msg, bool pretty = false);
bool ReadJson(std::string_view json,
              ModuleMessage* out,
              std::string* error = nullptr);

std::string WriteJson(const Command& cmd, bool pretty = false);
bool ReadJson(std::string_view json,
              Command* out,
              std::string* error = nullptr);

std::string WriteJson(const OptionFacts& facts, bool pretty = false);
bool ReadJson(std::string_view json,
              OptionFacts* out,
              std::string* error = nullptr);

// Checks visibility rules (HOVER fields only inside base.tooltips, OPEN panels
// only when listed in base.ui.open_panels, MODEL units only carry model
// fields).
std::vector<std::string> ValidateSnapshotVisibility(const Snapshot& snap);

// JSON object {"events": [...], "prompts": [...], "fields": [...]} reflected
// from Snapshot/Event/PromptEvent for a client-event audit (docs/design.md,
// "Client-event coverage").
std::string ReflectedSchemaJson();

// 16 hex digits naming this build's wire schema: a hash of kWireVersion and
// every field path of ModuleMessage and Command. Two builds whose hashes differ
// added, renamed or removed a field the other doesn't know.
const std::string& SchemaHash();
// This build's hello (wire_version, schema_hash) for `role`.
Hello MakeHello(std::string_view role);
// Why `peer` can't talk to this build ("" when it can): names both versions or
// schema hashes and says to rebuild the one that is out of date.
std::string HelloMismatch(const Hello& peer);

}  // namespace wlb::snapshot

#endif  // WLB_SNAPSHOT_SNAPSHOT_H_
