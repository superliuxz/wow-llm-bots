// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/order_evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <flat_set>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "grammar/orders.wlb.h"
#include "plan/orders.h"
#include "plan/triggers.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

bool NameEq(std::string_view a, std::string_view b) {
  return !a.empty() && !b.empty() && base::EqualsCaseInsensitiveASCII(a, b);
}

std::string LowerKey(std::string_view s) {
  return base::ToLowerASCII(s);
}

template <typename T, typename U>
bool Compare(T lhs, grammar::Cmp cmp, U rhs) {
  switch (cmp) {
    case grammar::Cmp::kLess:
      return static_cast<double>(lhs) < static_cast<double>(rhs);
    case grammar::Cmp::kGreater:
      return static_cast<double>(lhs) > static_cast<double>(rhs);
  }
  return false;
}

pb::RaidMarker ToRaidMarker(grammar::Icon icon) {
  switch (icon) {
    case grammar::Icon::kSkull:
      return pb::RaidMarker::SKULL;
    case grammar::Icon::kCross:
      return pb::RaidMarker::CROSS;
    case grammar::Icon::kSquare:
      return pb::RaidMarker::SQUARE;
    case grammar::Icon::kMoon:
      return pb::RaidMarker::MOON;
    case grammar::Icon::kTriangle:
      return pb::RaidMarker::TRIANGLE;
    case grammar::Icon::kDiamond:
      return pb::RaidMarker::DIAMOND;
    case grammar::Icon::kCircle:
      return pb::RaidMarker::CIRCLE;
    case grammar::Icon::kStar:
      return pb::RaidMarker::STAR;
  }
  return pb::RaidMarker::RAID_MARKER_UNSPECIFIED;
}

std::string_view AreaName(const pb::Area& area) {
  if (!area.name.empty()) {
    return area.name;
  }
  if (area.spell && !area.spell->name.empty()) {
    return area.spell->name;
  }
  return {};
}

bool IsZeroVec(const pb::Vec3& v) {
  return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
}

float Dist3D(const pb::Vec3& a, const pb::Vec3& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A lightweight handle to a unit, object, area, or self in the current
// snapshot. Only valid during a single `OnSnapshot` call.
struct Entity {
  std::string guid;
  std::string name;
  const pb::Unit* unit = nullptr;
  const pb::GameObject* object = nullptr;
  const pb::Area* area = nullptr;
  bool is_self = false;
  std::optional<pb::Vec3> position;
  std::optional<float> dist_from_me_yd;
};

Entity MakeSelfEntity(const pb::Snapshot& snap) {
  Entity e;
  e.guid = snap.base.self.unit.guid;
  e.name = snap.base.self.unit.name;
  e.unit = &snap.base.self.unit;
  e.is_self = true;
  e.position = snap.base.where.position;
  e.dist_from_me_yd = 0.0f;
  return e;
}

Entity MakeUnitEntity(const pb::Unit& u) {
  Entity e;
  e.guid = u.guid;
  e.name = u.name;
  e.unit = &u;
  if (!IsZeroVec(u.rel.position)) {
    e.position = u.rel.position;
  }
  e.dist_from_me_yd = u.rel.dist_yd;
  return e;
}

Entity MakeObjectEntity(const pb::GameObject& o) {
  Entity e;
  e.guid = o.guid;
  e.name = o.name;
  e.object = &o;
  if (!IsZeroVec(o.rel.position)) {
    e.position = o.rel.position;
  }
  e.dist_from_me_yd = o.rel.dist_yd;
  return e;
}

Entity MakeAreaEntity(const pb::Area& a) {
  Entity e;
  e.guid = a.guid;
  e.name = std::string(AreaName(a));
  e.area = &a;
  if (a.rel) {
    if (!IsZeroVec(a.rel->position)) {
      e.position = a.rel->position;
    }
    e.dist_from_me_yd = a.rel->dist_yd;
  } else if (a.inside_me) {
    e.dist_from_me_yd = 0.0f;
  }
  return e;
}

bool SameEntity(const Entity& a, const Entity& b) {
  if (a.is_self && b.is_self) {
    return true;
  }
  if (a.is_self != b.is_self) {
    return !a.guid.empty() && a.guid == b.guid;
  }
  if (!a.guid.empty() && !b.guid.empty()) {
    return a.guid == b.guid;
  }
  return a.unit && a.unit == b.unit;
}

std::optional<float> DistanceBetween(const Entity& a, const Entity& b) {
  if (SameEntity(a, b)) {
    return 0.0f;
  }
  if (a.is_self) {
    if (b.dist_from_me_yd) {
      return *b.dist_from_me_yd;
    }
    if (a.position && b.position) {
      return Dist3D(*a.position, *b.position);
    }
    return std::nullopt;
  }
  if (b.is_self) {
    return DistanceBetween(b, a);
  }
  if (a.position && b.position) {
    return Dist3D(*a.position, *b.position);
  }
  return std::nullopt;
}

bool UnitMatchesRef(const Entity& e, std::string_view ref) {
  if (ref.empty()) {
    return false;
  }
  if (!e.guid.empty() && e.guid == ref) {
    return true;
  }
  return NameEq(e.name, ref);
}

bool IsUnitCasting(const pb::Unit& u, std::string_view mech) {
  if (!u.activity.cast || !u.activity.cast->spell) {
    return false;
  }
  if (u.activity.state != pb::Activity::STATE_UNSPECIFIED &&
      u.activity.state != pb::Activity::CASTING &&
      u.activity.state != pb::Activity::CHANNELING) {
    return false;
  }
  return NameEq(u.activity.cast->spell->name, mech);
}

const pb::Aura* FindAura(const pb::Unit& u, std::string_view mech) {
  for (const pb::Aura& aura : u.auras) {
    if (NameEq(aura.name, mech)) {
      return &aura;
    }
  }
  return nullptr;
}

bool IsGroupMember(const pb::Snapshot& snap, const pb::Unit& u) {
  if (u.group.has_value() || (u.player && u.player->in_my_group)) {
    return true;
  }
  if (snap.ext.party) {
    for (const pb::Unit& m : snap.ext.party->members) {
      if ((!u.guid.empty() && m.guid == u.guid) || NameEq(m.name, u.name)) {
        return true;
      }
    }
  }
  if (snap.ext.raid) {
    for (const pb::Unit& m : snap.ext.raid->members) {
      if ((!u.guid.empty() && m.guid == u.guid) || NameEq(m.name, u.name)) {
        return true;
      }
    }
  }
  return false;
}

bool IsFriendlyUnit(const pb::Snapshot& snap, const pb::Unit& u) {
  return u.reaction == pb::Unit::FRIENDLY || IsGroupMember(snap, u);
}

// Flattened, deduplicated view of all units in a snapshot for fast lookup.
struct SnapshotView {
  const pb::Snapshot& snap;
  base::TimeTicks now;
  base::TimeTicks installed_at;
  const std::flat_map<std::string, std::pair<base::TimeTicks, Entity>>*
      last_casts = nullptr;
  const std::flat_map<std::string, std::pair<base::TimeTicks, Entity>>*
      last_appears = nullptr;
  std::optional<base::TimeTicks> order_last_every;

  Entity self;
  std::vector<const pb::Unit*> units;

  explicit SnapshotView(const pb::Snapshot& s,
                        base::TimeTicks t,
                        base::TimeTicks installed)
      : snap(s), now(t), installed_at(installed), self(MakeSelfEntity(s)) {
    std::flat_set<std::string> seen_guids;
    auto add_unit = [&](const pb::Unit& u) {
      if (!u.guid.empty()) {
        if (!self.guid.empty() && u.guid == self.guid) {
          return;
        }
        if (!seen_guids.insert(u.guid).second) {
          return;
        }
      }
      units.push_back(&u);
    };

    for (const pb::Unit& u : s.base.nearby.units) {
      add_unit(u);
    }
    if (s.ext.encounter && s.ext.encounter->boss) {
      add_unit(*s.ext.encounter->boss);
    }
    if (s.ext.target) {
      if (s.ext.target->target) {
        add_unit(*s.ext.target->target);
      }
      if (s.ext.target->focus) {
        add_unit(*s.ext.target->focus);
      }
    }
    if (s.ext.party) {
      for (const pb::Unit& u : s.ext.party->members) {
        add_unit(u);
      }
    }
    if (s.ext.raid) {
      for (const pb::Unit& u : s.ext.raid->members) {
        add_unit(u);
      }
    }
    if (s.ext.combat) {
      for (const pb::Unit& u : s.ext.combat->attackers) {
        add_unit(u);
      }
    }
    if (s.ext.pet && s.ext.pet->pet) {
      add_unit(*s.ext.pet->pet);
    }
  }

  // Finds the richest Entity for a unit reference (preferring `units` over a
  // sparse extension copy).
  Entity RichUnit(const pb::Unit& u) const {
    if (!self.guid.empty() && u.guid == self.guid) {
      return self;
    }
    if (!u.guid.empty()) {
      for (const pb::Unit* cand : units) {
        if (cand->guid == u.guid) {
          return MakeUnitEntity(*cand);
        }
      }
    }
    return MakeUnitEntity(u);
  }

  std::optional<Entity> FindByGuidOrName(std::string_view guid,
                                         std::string_view name) const {
    if (!guid.empty() && guid == self.guid) {
      return self;
    }
    for (const pb::Unit* u : units) {
      if (!guid.empty() && u->guid == guid) {
        return MakeUnitEntity(*u);
      }
    }
    for (const pb::GameObject& o : snap.base.nearby.objects) {
      if (!guid.empty() && o.guid == guid) {
        return MakeObjectEntity(o);
      }
    }
    for (const pb::Area& a : snap.base.nearby.areas) {
      if (!guid.empty() && a.guid == guid) {
        return MakeAreaEntity(a);
      }
    }
    if (!name.empty()) {
      if (NameEq(self.name, name)) {
        return self;
      }
      for (const pb::Unit* u : units) {
        if (!u->dead && NameEq(u->name, name)) {
          return MakeUnitEntity(*u);
        }
      }
      for (const pb::GameObject& o : snap.base.nearby.objects) {
        if (NameEq(o.name, name)) {
          return MakeObjectEntity(o);
        }
      }
      for (const pb::Area& a : snap.base.nearby.areas) {
        if (NameEq(AreaName(a), name)) {
          return MakeAreaEntity(a);
        }
      }
    }
    return std::nullopt;
  }

  std::vector<Entity> FindTanks() const {
    std::vector<Entity> tanks;
    auto add = [&](const Entity& e) {
      for (const Entity& existing : tanks) {
        if (SameEntity(existing, e)) {
          return;
        }
      }
      tanks.push_back(e);
    };
    // Raid main tanks first (preserves raid order for tank vs offtank).
    if (snap.ext.raid) {
      for (const std::string& ref : snap.ext.raid->main_tanks) {
        if (UnitMatchesRef(self, ref)) {
          add(self);
          continue;
        }
        for (const pb::Unit* u : units) {
          Entity e = MakeUnitEntity(*u);
          if (UnitMatchesRef(e, ref)) {
            add(e);
            break;
          }
        }
      }
    }
    // Units flagged `group.main_tank`.
    if (snap.base.self.unit.group && snap.base.self.unit.group->main_tank) {
      add(self);
    }
    for (const pb::Unit* u : units) {
      if (u->group && u->group->main_tank) {
        add(MakeUnitEntity(*u));
      }
    }
    // Fallback: units with `group.role == TANK`.
    if (snap.base.self.unit.group &&
        snap.base.self.unit.group->role == pb::Role::TANK) {
      add(self);
    }
    for (const pb::Unit* u : units) {
      if (u->group && u->group->role == pb::Role::TANK) {
        add(MakeUnitEntity(*u));
      }
    }
    return tanks;
  }
};

std::vector<Entity> ResolveWho(const SnapshotView& view,
                               grammar::Who who,
                               const std::optional<Entity>& it) {
  std::vector<Entity> out;
  switch (who) {
    case grammar::Who::kMe:
      out.push_back(view.self);
      break;
    case grammar::Who::kIt:
      if (it) {
        out.push_back(*it);
      }
      break;
    case grammar::Who::kTank: {
      std::vector<Entity> tanks = view.FindTanks();
      if (!tanks.empty()) {
        out.push_back(std::move(tanks.front()));
      }
      break;
    }
    case grammar::Who::kOfftank: {
      std::vector<Entity> tanks = view.FindTanks();
      for (size_t i = 1; i < tanks.size(); ++i) {
        out.push_back(std::move(tanks[i]));
      }
      break;
    }
    case grammar::Who::kHealer:
      if (view.snap.base.self.unit.group &&
          view.snap.base.self.unit.group->role == pb::Role::HEALER) {
        out.push_back(view.self);
      }
      for (const pb::Unit* u : view.units) {
        if (u->group && u->group->role == pb::Role::HEALER) {
          out.push_back(MakeUnitEntity(*u));
        }
      }
      break;
    case grammar::Who::kLeader:
      if (view.snap.base.self.group_leader ||
          (view.snap.base.self.unit.group &&
           view.snap.base.self.unit.group->leader)) {
        out.push_back(view.self);
        break;
      }
      if (view.snap.ext.party && !view.snap.ext.party->leader.empty()) {
        const std::string& leader = view.snap.ext.party->leader;
        if (UnitMatchesRef(view.self, leader)) {
          out.push_back(view.self);
          break;
        }
        for (const pb::Unit* u : view.units) {
          Entity e = MakeUnitEntity(*u);
          if (UnitMatchesRef(e, leader)) {
            out.push_back(std::move(e));
            break;
          }
        }
        if (!out.empty()) {
          break;
        }
      }
      for (const pb::Unit* u : view.units) {
        if (u->group && u->group->leader) {
          out.push_back(MakeUnitEntity(*u));
          break;
        }
      }
      break;
    case grammar::Who::kBoss:
      if (view.snap.ext.encounter && view.snap.ext.encounter->boss) {
        out.push_back(view.RichUnit(*view.snap.ext.encounter->boss));
      }
      for (const pb::Unit* u : view.units) {
        if (u->boss.has_value() || u->classification == pb::Unit::WORLD_BOSS) {
          Entity e = MakeUnitEntity(*u);
          if (std::ranges::none_of(
                  out, [&](const Entity& x) { return SameEntity(x, e); })) {
            out.push_back(std::move(e));
          }
        }
      }
      break;
    case grammar::Who::kAnyone:
      // Any friendly raid/party member (never hostile enemies).
      out.push_back(view.self);
      for (const pb::Unit* u : view.units) {
        if (!u->dead &&
            (IsGroupMember(view.snap, *u) ||
             (u->kind == pb::Unit::PLAYER && IsFriendlyUnit(view.snap, *u)))) {
          out.push_back(MakeUnitEntity(*u));
        }
      }
      break;
  }
  return out;
}

template <typename Pred>
std::optional<Entity> NearestUnit(const SnapshotView& view, Pred pred) {
  std::optional<Entity> best;
  float best_dist = std::numeric_limits<float>::infinity();
  for (const pb::Unit* u : view.units) {
    if (u->dead || !pred(*u)) {
      continue;
    }
    Entity e = MakeUnitEntity(*u);
    float d = DistanceBetween(view.self, e)
                  .value_or(std::numeric_limits<float>::infinity());
    if (!best || d < best_dist) {
      best = std::move(e);
      best_dist = d;
    }
  }
  return best;
}

std::vector<Entity> ResolveNamed(const SnapshotView& view,
                                 std::string_view name) {
  std::vector<Entity> out;
  for (const pb::Unit* u : view.units) {
    if (!u->dead && NameEq(u->name, name)) {
      out.push_back(MakeUnitEntity(*u));
    }
  }
  for (const pb::GameObject& o : view.snap.base.nearby.objects) {
    if (NameEq(o.name, name)) {
      out.push_back(MakeObjectEntity(o));
    }
  }
  for (const pb::Area& a : view.snap.base.nearby.areas) {
    if (NameEq(AreaName(a), name)) {
      out.push_back(MakeAreaEntity(a));
    }
  }
  return out;
}

std::vector<Entity> ResolveThing(const SnapshotView& view,
                                 const Thing& thing,
                                 const std::optional<Entity>& it) {
  return std::visit(
      [&](const auto& t) -> std::vector<Entity> {
        using T = std::decay_t<decltype(t)>;
        std::vector<Entity> out;
        if constexpr (std::is_same_v<T, grammar::ThingWho>) {
          return ResolveWho(view, t.who, it);
        } else if constexpr (std::is_same_v<T, grammar::ThingMyTarget>) {
          if (view.snap.ext.target && view.snap.ext.target->target) {
            out.push_back(view.RichUnit(*view.snap.ext.target->target));
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingFocus>) {
          if (view.snap.ext.target && view.snap.ext.target->focus) {
            out.push_back(view.RichUnit(*view.snap.ext.target->focus));
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingMarker>) {
          const pb::RaidMarker marker = ToRaidMarker(t.icon);
          if (view.snap.base.self.unit.raid_marker == marker) {
            out.push_back(view.self);
          }
          for (const pb::Unit* u : view.units) {
            if (!u->dead && u->raid_marker == marker) {
              out.push_back(MakeUnitEntity(*u));
            }
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingNearest>) {
          std::optional<Entity> best = std::visit(
              [&](const auto& k) -> std::optional<Entity> {
                using K = std::decay_t<decltype(k)>;
                if constexpr (std::is_same_v<K, grammar::NearestKindAlly>) {
                  return NearestUnit(view, [&](const pb::Unit& u) {
                    return IsFriendlyUnit(view.snap, u);
                  });
                } else if constexpr (std::is_same_v<
                                         K, grammar::NearestKindPlayer>) {
                  return NearestUnit(view, [](const pb::Unit& u) {
                    return u.kind == pb::Unit::PLAYER;
                  });
                } else if constexpr (std::is_same_v<
                                         K, grammar::NearestKindEnemy>) {
                  return NearestUnit(view, [](const pb::Unit& u) {
                    return u.reaction == pb::Unit::HOSTILE;
                  });
                } else if constexpr (std::is_same_v<
                                         K, grammar::NearestKindNamed>) {
                  std::vector<Entity> named = ResolveNamed(view, k.name);
                  std::optional<Entity> closest;
                  float best_dist = std::numeric_limits<float>::infinity();
                  for (Entity& e : named) {
                    float d =
                        DistanceBetween(view.self, e)
                            .value_or(std::numeric_limits<float>::infinity());
                    if (!closest || d < best_dist) {
                      closest = std::move(e);
                      best_dist = d;
                    }
                  }
                  return closest;
                }
              },
              t.kind.value);
          if (best) {
            out.push_back(std::move(*best));
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingAllyWith>) {
          if (FindAura(view.snap.base.self.unit, t.mech)) {
            out.push_back(view.self);
          }
          for (const pb::Unit* u : view.units) {
            if (!u->dead && IsFriendlyUnit(view.snap, *u) &&
                FindAura(*u, t.mech)) {
              out.push_back(MakeUnitEntity(*u));
            }
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingGroup>) {
          for (const pb::Unit* u : view.units) {
            if (!u->dead && IsGroupMember(view.snap, *u)) {
              out.push_back(MakeUnitEntity(*u));
            }
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingAllies>) {
          for (const pb::Unit* u : view.units) {
            if (!u->dead && IsFriendlyUnit(view.snap, *u)) {
              out.push_back(MakeUnitEntity(*u));
            }
          }
        } else if constexpr (std::is_same_v<T, grammar::ThingNamed>) {
          return ResolveNamed(view, t.name);
        }
        return out;
      },
      thing.value);
}

std::optional<double> EntityHealthPct(const pb::Snapshot& snap,
                                      const Entity& e) {
  if (e.is_self) {
    if (snap.base.self.unit.health_pct) {
      return *snap.base.self.unit.health_pct;
    }
    if (snap.base.self.health_max > 0) {
      return 100.0 * static_cast<double>(snap.base.self.health) /
             static_cast<double>(snap.base.self.health_max);
    }
    return std::nullopt;
  }
  if (e.unit && e.unit->health_pct) {
    return *e.unit->health_pct;
  }
  if (e.object && e.object->health_pct) {
    return *e.object->health_pct;
  }
  return std::nullopt;
}

std::optional<bool> EntityInLos(const Entity& e) {
  if (e.is_self) {
    return true;
  }
  if (e.unit) {
    return e.unit->rel.in_los;
  }
  if (e.object) {
    return e.object->rel.in_los;
  }
  if (e.area && e.area->rel) {
    return e.area->rel->in_los;
  }
  return std::nullopt;
}

struct CheckResult {
  bool matched = false;
  std::optional<Entity> bind_it;
};

CheckResult EvalCheck(const SnapshotView& view,
                      const Check& check,
                      const std::optional<Entity>& it) {
  return std::visit(
      [&](const auto& c) -> CheckResult {
        using C = std::decay_t<decltype(c)>;

        if constexpr (std::is_same_v<C, grammar::CheckStacks>) {
          for (const Entity& e : ResolveWho(view, c.who, it)) {
            if (!e.unit) {
              continue;
            }
            const pb::Aura* aura = FindAura(*e.unit, c.mech);
            if (!aura) {
              continue;
            }
            const int stacks =
                aura->stacks == 0 ? 1 : static_cast<int>(aura->stacks);
            if (Compare(stacks, c.cmp, c.n)) {
              return {true, e};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckEnds>) {
          for (const Entity& e : ResolveWho(view, c.who, it)) {
            if (!e.unit) {
              continue;
            }
            const pb::Aura* aura = FindAura(*e.unit, c.mech);
            if (aura && aura->remaining_s && *aura->remaining_s >= 0.0f &&
                *aura->remaining_s <= c.s) {
              return {true, e};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckOn>) {
          for (const Entity& e : ResolveWho(view, c.who, it)) {
            if (e.unit && FindAura(*e.unit, c.mech)) {
              return {true, e};
            }
          }
          // Ground pools/areas the bot is standing inside also satisfy
          // `<mech> on me` (e.g. Defile).
          if (c.who == grammar::Who::kMe) {
            for (const pb::Area& area : view.snap.base.nearby.areas) {
              if (area.inside_me && NameEq(AreaName(area), c.mech)) {
                return {true, MakeAreaEntity(area)};
              }
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckCastOn>) {
          const std::vector<Entity> targets = ResolveWho(view, c.who, it);
          if (targets.empty()) {
            return {};
          }
          auto matches_target = [&](const pb::Unit& caster) {
            const std::string& cast_target =
                (caster.activity.cast && !caster.activity.cast->target.empty())
                    ? caster.activity.cast->target
                    : caster.target;
            for (const Entity& t : targets) {
              if (t.is_self && caster.targeting_me &&
                  (cast_target.empty() || UnitMatchesRef(t, cast_target))) {
                return true;
              }
              if (!cast_target.empty() && UnitMatchesRef(t, cast_target)) {
                return true;
              }
            }
            return false;
          };
          for (const pb::Unit* u : view.units) {
            if (!u->dead && IsUnitCasting(*u, c.mech) && matches_target(*u)) {
              return {true, MakeUnitEntity(*u)};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckCast>) {
          for (const pb::Unit* u : view.units) {
            if (!u->dead && IsUnitCasting(*u, c.mech)) {
              return {true, MakeUnitEntity(*u)};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckCastInLast>) {
          if (view.last_casts) {
            auto found = view.last_casts->find(LowerKey(c.mech));
            if (found != view.last_casts->end() &&
                (view.now - found->second.first) <= base::Seconds(c.s)) {
              return {true, found->second.second};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckAppears>) {
          std::vector<Entity> matches = ResolveNamed(view, c.mech);
          if (matches.empty()) {
            return {};
          }
          // Bind `it` to the nearest matching entity.
          const Entity* best = &matches.front();
          float best_dist =
              DistanceBetween(view.self, *best)
                  .value_or(std::numeric_limits<float>::infinity());
          for (size_t i = 1; i < matches.size(); ++i) {
            float d = DistanceBetween(view.self, matches[i])
                          .value_or(std::numeric_limits<float>::infinity());
            if (d < best_dist) {
              best = &matches[i];
              best_dist = d;
            }
          }
          return {true, *best};

        } else if constexpr (std::is_same_v<C, grammar::CheckAppearsIn>) {
          if (view.last_appears) {
            auto found = view.last_appears->find(LowerKey(c.mech));
            if (found != view.last_appears->end() &&
                (view.now - found->second.first) <= base::Seconds(c.s)) {
              return {true, found->second.second};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckCount>) {
          std::vector<Entity> matches = ResolveNamed(view, c.mech);
          if (Compare(static_cast<int>(matches.size()), c.cmp, c.n)) {
            std::optional<Entity> bound;
            if (!matches.empty()) {
              bound = matches.front();
            }
            return {true, std::move(bound)};
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckWithin> ||
                             std::is_same_v<C, grammar::CheckBeyond>) {
          const bool want_within = std::is_same_v<C, grammar::CheckWithin>;
          const std::vector<Entity> as = ResolveThing(view, c.a, it);
          const std::vector<Entity> bs = c.b ? ResolveThing(view, *c.b, it)
                                             : std::vector<Entity>{view.self};
          for (const Entity& ea : as) {
            for (const Entity& eb : bs) {
              if (SameEntity(ea, eb)) {
                continue;
              }
              std::optional<float> d = DistanceBetween(ea, eb);
              if (!d) {
                continue;
              }
              const bool ok = want_within ? (*d <= c.yd) : (*d > c.yd);
              if (ok) {
                return {true, ea.is_self ? eb : ea};
              }
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckTargets>) {
          const std::vector<Entity> as = ResolveThing(view, c.a, it);
          const std::vector<Entity> bs = ResolveThing(view, c.b, it);
          for (const Entity& ea : as) {
            for (const Entity& eb : bs) {
              bool targeting = false;
              if (ea.is_self) {
                if (view.snap.ext.target && view.snap.ext.target->target) {
                  Entity my_target =
                      view.RichUnit(*view.snap.ext.target->target);
                  targeting = SameEntity(my_target, eb) ||
                              UnitMatchesRef(eb, my_target.guid) ||
                              UnitMatchesRef(eb, my_target.name);
                }
              } else if (ea.unit) {
                if (eb.is_self && ea.unit->targeting_me) {
                  targeting = true;
                } else if (!ea.unit->target.empty() &&
                           UnitMatchesRef(eb, ea.unit->target)) {
                  targeting = true;
                }
              }
              if (targeting) {
                return {true, ea.is_self ? eb : ea};
              }
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckHp>) {
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            std::optional<double> hp = EntityHealthPct(view.snap, ea);
            if (hp && Compare(*hp, c.cmp, c.pct)) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckManaPoints>) {
          const pb::Self& s = view.snap.base.self;
          if (s.power_type == pb::Self::MANA) {
            return {Compare(s.power, c.cmp, c.n), std::nullopt};
          }
          if (s.alt_mana_max > 0) {
            return {Compare(s.alt_mana, c.cmp, c.n), std::nullopt};
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckMana>) {
          const pb::Self& s = view.snap.base.self;
          uint32_t cur = 0;
          uint32_t max = 0;
          if (s.power_type == pb::Self::MANA) {
            cur = s.power;
            max = s.power_max;
          } else if (s.alt_mana_max > 0) {
            cur = s.alt_mana;
            max = s.alt_mana_max;
          }
          if (max == 0) {
            return {};
          }
          const double pct =
              100.0 * static_cast<double>(cur) / static_cast<double>(max);
          return {Compare(pct, c.cmp, c.pct), std::nullopt};

        } else if constexpr (std::is_same_v<C, grammar::CheckIsMarked>) {
          const pb::RaidMarker want = ToRaidMarker(c.icon);
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            if (ea.unit && ea.unit->raid_marker == want) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckIs>) {
          auto has_visual = [](const pb::Unit& u, pb::Unit::VisualState vs) {
            return std::ranges::find(u.visual_state, vs) !=
                   u.visual_state.end();
          };
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            if (!ea.unit) {
              continue;
            }
            bool ok = false;
            switch (c.state) {
              case grammar::State::kFlying:
                ok = has_visual(*ea.unit, pb::Unit::FLYING);
                break;
              case grammar::State::kGrounded:
                ok = !has_visual(*ea.unit, pb::Unit::FLYING) &&
                     !has_visual(*ea.unit, pb::Unit::SUBMERGED);
                break;
              case grammar::State::kUntargetable:
                ok = has_visual(*ea.unit, pb::Unit::UNTARGETABLE);
                break;
              case grammar::State::kSubmerged:
                ok = has_visual(*ea.unit, pb::Unit::SUBMERGED);
                break;
            }
            if (ok) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckInSight>) {
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            if (EntityInLos(ea).value_or(false)) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckOutOfSight>) {
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            std::optional<bool> los = EntityInLos(ea);
            if (los.has_value() && !*los) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckMeBehind>) {
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            if (ea.unit && ea.unit->rel.behind_it) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckMyTargetIs>) {
          if (!view.snap.ext.target || !view.snap.ext.target->target) {
            return {};
          }
          const Entity my_target = view.RichUnit(*view.snap.ext.target->target);
          for (const Entity& ea : ResolveThing(view, c.a, it)) {
            if (SameEntity(my_target, ea) ||
                (!ea.name.empty() && NameEq(my_target.name, ea.name))) {
              return {true, ea};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckMeHas>) {
          if (view.snap.ext.bags) {
            for (const pb::Item& item : view.snap.ext.bags->items) {
              if (item.count > 0 && NameEq(item.name, c.name)) {
                return {true, std::nullopt};
              }
            }
          }
          if (view.snap.ext.character) {
            for (const pb::Item& item : view.snap.ext.character->equipped) {
              if (NameEq(item.name, c.name)) {
                return {true, std::nullopt};
              }
            }
          }
          if (view.snap.ext.spellbook) {
            for (const pb::Spell& spell : view.snap.ext.spellbook->spells) {
              if (NameEq(spell.name, c.name)) {
                return {true, std::nullopt};
              }
            }
          }
          if (view.snap.ext.action_bars) {
            for (const pb::Spell& spell : view.snap.ext.action_bars->buttons) {
              if (NameEq(spell.name, c.name)) {
                return {true, std::nullopt};
              }
            }
          }
          if (FindAura(view.snap.base.self.unit, c.name)) {
            return {true, std::nullopt};
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckMe>) {
          switch (c.state) {
            case grammar::MyState::kInVehicle:
              return {view.snap.ext.vehicle.has_value(), std::nullopt};
            case grammar::MyState::kMounted:
              return {view.snap.base.self.mounted, std::nullopt};
            case grammar::MyState::kSwimming:
              return {view.snap.base.self.swimming, std::nullopt};
            case grammar::MyState::kNearestToIt: {
              if (!it) {
                return {};
              }
              std::optional<float> my_dist = DistanceBetween(view.self, *it);
              if (!my_dist) {
                return {};
              }
              for (const pb::Unit* u : view.units) {
                if (u->dead || !IsFriendlyUnit(view.snap, *u)) {
                  continue;
                }
                Entity ally = MakeUnitEntity(*u);
                if (SameEntity(ally, *it)) {
                  continue;
                }
                std::optional<float> ally_dist = DistanceBetween(ally, *it);
                if (ally_dist && *ally_dist < *my_dist) {
                  return {false, std::nullopt};
                }
              }
              return {true, std::nullopt};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckPhase>) {
          if (view.snap.ext.encounter && view.snap.ext.encounter->boss &&
              view.snap.ext.encounter->boss->boss &&
              view.snap.ext.encounter->boss->boss->phase ==
                  static_cast<uint32_t>(c.n)) {
            return {true, std::nullopt};
          }
          for (const pb::Unit* u : view.units) {
            if (u->boss && u->boss->phase == static_cast<uint32_t>(c.n)) {
              return {true, std::nullopt};
            }
          }
          return {};

        } else if constexpr (std::is_same_v<C, grammar::CheckAfter>) {
          return {(view.now - view.installed_at) >= base::Seconds(c.s),
                  std::nullopt};

        } else if constexpr (std::is_same_v<C, grammar::CheckEvery>) {
          const base::TimeTicks ref =
              view.order_last_every.value_or(view.installed_at);
          return {(view.now - ref) >= base::Seconds(c.s), std::nullopt};
        }
      },
      check.value);
}

bool OrderHasEvery(const Order& order) {
  const auto* all = std::get_if<grammar::WhenAll>(&order.when.value);
  if (!all) {
    return false;
  }
  for (const Condition& cond : all->conds) {
    if (!cond.negated &&
        std::holds_alternative<grammar::CheckEvery>(cond.check.value)) {
      return true;
    }
  }
  return false;
}

struct WhenMatch {
  bool matched = false;
  std::optional<Entity> it;
};

WhenMatch EvalWhen(const SnapshotView& view, const When& when) {
  if (std::holds_alternative<grammar::WhenAlways>(when.value)) {
    return {true, std::nullopt};
  }
  const auto& all = std::get<grammar::WhenAll>(when.value);
  std::optional<Entity> it;
  for (const Condition& cond : all.conds) {
    CheckResult r = EvalCheck(view, cond.check, it);
    const bool ok = cond.negated ? !r.matched : r.matched;
    if (!ok) {
      return {};
    }
    if (!cond.negated && !it && r.bind_it) {
      it = std::move(r.bind_it);
    }
  }
  return {true, std::move(it)};
}

// Resolves the explicit target `Thing` of an `Act` against `view` using the
// exact same rules as condition evaluation, returning the chosen entity's GUID
// so the module acts on the same unit.
std::string ResolveActThingGuid(const SnapshotView& view,
                                const Act& act,
                                const std::optional<Entity>& it) {
  const Thing* thing = std::visit(
      [](const auto& a) -> const Thing* {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, grammar::ActTo>) {
          return std::visit([](const auto& p) -> const Thing* { return &p.t; },
                            a.place.value);
        } else if constexpr (std::is_same_v<A, grammar::ActAway> ||
                             std::is_same_v<A, grammar::ActUse>) {
          return a.t ? &*a.t : nullptr;
        } else if constexpr (std::is_same_v<A, grammar::ActTarget> ||
                             std::is_same_v<A, grammar::ActAvoid> ||
                             std::is_same_v<A, grammar::ActFace> ||
                             std::is_same_v<A, grammar::ActFaceAway> ||
                             std::is_same_v<A, grammar::ActMark> ||
                             std::is_same_v<A, grammar::ActInteract> ||
                             std::is_same_v<A, grammar::ActBoard> ||
                             std::is_same_v<A, grammar::ActPetAttack> ||
                             std::is_same_v<A, grammar::ActKite>) {
          return &a.t;
        } else {
          return nullptr;
        }
      },
      act.value);
  if (!thing) {
    return {};
  }
  if (const auto* w = std::get_if<grammar::ThingWho>(&thing->value);
      w && (w->who == grammar::Who::kMe || w->who == grammar::Who::kIt)) {
    return {};
  }
  if (std::holds_alternative<grammar::ThingFocus>(thing->value)) {
    return {};
  }
  const std::vector<Entity> resolved = ResolveThing(view, *thing, it);
  if (resolved.empty()) {
    return {};
  }
  // Prefer the entity the condition matched (`it`) when it belongs to the
  // resolved set (e.g. `Mottled Boar targets me -> kite Mottled Boar`).
  if (it) {
    for (const Entity& e : resolved) {
      if (!e.guid.empty() && SameEntity(e, *it)) {
        return e.guid;
      }
    }
  }
  // Otherwise pick the nearest matching entity instead of unordered map order.
  const Entity* best = nullptr;
  float best_dist = std::numeric_limits<float>::infinity();
  for (const Entity& e : resolved) {
    if (e.guid.empty()) {
      continue;
    }
    const float d = DistanceBetween(view.self, e)
                        .value_or(std::numeric_limits<float>::infinity());
    if (!best || d < best_dist) {
      best = &e;
      best_dist = d;
    }
  }
  return best ? best->guid : resolved.front().guid;
}

}  // namespace

OrderEvaluator::OrderEvaluator(base::TimeDelta step_timeout)
    : step_timeout_(step_timeout) {}

bool OrderEvaluator::SetOrders(std::vector<Order> orders, base::TimeTicks now) {
  // Preserve edge latches, `after Ns` / `every Ns` clocks, and any currently
  // running step whose order text did not change across a replan, so a held
  // condition (`hp<30 -> wake`) does not fire again on every new plan and
  // timer orders (`after Ns`, first `every Ns`) do not restart on frequent
  // replans.
  const bool was_moving = ActiveStepMoves();
  std::string prev_active_key;
  if (was_moving && pending_command_id_ != 0 && active_order_ >= 0 &&
      static_cast<size_t>(active_order_) < orders_.size()) {
    prev_active_key = FormatOrder(orders_[static_cast<size_t>(active_order_)]);
  }
  std::flat_set<std::string> prev_latched;
  std::flat_map<std::string, base::TimeTicks> prev_installed;
  std::flat_map<std::string, base::TimeTicks> prev_every;
  for (size_t i = 0; i < orders_.size(); ++i) {
    const std::string key = FormatOrder(orders_[i]);
    if (i < order_latched_.size() && order_latched_[i]) {
      prev_latched.insert(key);
    }
    if (i < order_installed_at_.size()) {
      prev_installed[key] = order_installed_at_[i];
    }
    if (i < last_every_fired_.size() && last_every_fired_[i].has_value()) {
      prev_every[key] = *last_every_fired_[i];
    }
  }
  orders_ = std::move(orders);
  order_installed_at_.assign(orders_.size(), now);
  last_every_fired_.assign(orders_.size(), std::nullopt);
  order_latched_.assign(orders_.size(), false);
  int kept_active_idx = -1;
  for (size_t i = 0; i < orders_.size(); ++i) {
    const std::string key = FormatOrder(orders_[i]);
    if (prev_latched.contains(key)) {
      order_latched_[i] = true;
    }
    if (auto it = prev_installed.find(key); it != prev_installed.end()) {
      order_installed_at_[i] = it->second;
    }
    if (auto it = prev_every.find(key); it != prev_every.end()) {
      last_every_fired_[i] = it->second;
    }
    if (!prev_active_key.empty() && kept_active_idx < 0 &&
        key == prev_active_key) {
      kept_active_idx = static_cast<int>(i);
    }
  }
  if (kept_active_idx >= 0) {
    active_order_ = kept_active_idx;
    return false;
  }
  ResetActive();
  return was_moving;
}

bool OrderEvaluator::ActiveStepMoves() const {
  if (active_order_ < 0 ||
      static_cast<size_t>(active_order_) >= orders_.size()) {
    return false;
  }
  const Order& order = orders_[static_cast<size_t>(active_order_)];
  if (active_step_ < 0 ||
      static_cast<size_t>(active_step_) >= order.steps.size()) {
    return false;
  }
  const Act& act = order.steps[static_cast<size_t>(active_step_)];
  return std::holds_alternative<grammar::ActTo>(act.value) ||
         std::holds_alternative<grammar::ActAway>(act.value) ||
         std::holds_alternative<grammar::ActKite>(act.value) ||
         std::holds_alternative<grammar::ActInteract>(act.value) ||
         std::holds_alternative<grammar::ActBoard>(act.value);
}

void OrderEvaluator::Clear() {
  orders_.clear();
  order_installed_at_.clear();
  last_every_fired_.clear();
  order_latched_.clear();
  ResetActive();
}

void OrderEvaluator::ResetActive() {
  active_order_ = -1;
  active_step_ = 0;
  active_it_guid_.clear();
  active_it_name_.clear();
  step_started_at_ = base::TimeTicks();
  pending_command_id_ = 0;
}

std::optional<base::TimeDelta> OrderEvaluator::TimeoutForAct(
    const Act& act) const {
  if (!step_timeout_.is_positive()) {
    return std::nullopt;
  }
  if (std::holds_alternative<grammar::ActKite>(act.value)) {
    // Kiting is continuous until preempted or the kited unit dies/vanishes.
    return std::nullopt;
  }
  if (std::holds_alternative<grammar::ActTo>(act.value) ||
      std::holds_alternative<grammar::ActAway>(act.value) ||
      std::holds_alternative<grammar::ActInteract>(act.value) ||
      std::holds_alternative<grammar::ActBoard>(act.value)) {
    return std::max(step_timeout_, kLongOrderMoveTimeout);
  }
  return step_timeout_;
}

void OrderEvaluator::UpdateHistory(const pb::Snapshot& snapshot,
                                   base::TimeTicks now) {
  // Prune stale history entries so maps stay bounded over long sessions.
  auto prune = [&](std::flat_map<std::string, SeenEvent>& map) {
    std::erase_if(map, [&](const auto& kv) {
      return (now - kv.second.when) > kMaxOrderHistoryAge;
    });
  };
  prune(last_casts_);
  prune(last_appears_);

  // Record active casts from all units in the snapshot.
  auto record_cast = [&](const pb::Unit& u) {
    if (u.dead || !u.activity.cast || !u.activity.cast->spell) {
      return;
    }
    if (u.activity.state != pb::Activity::STATE_UNSPECIFIED &&
        u.activity.state != pb::Activity::CASTING &&
        u.activity.state != pb::Activity::CHANNELING) {
      return;
    }
    const std::string& spell = u.activity.cast->spell->name;
    if (!spell.empty()) {
      last_casts_[LowerKey(spell)] = {now, u.guid, u.name};
    }
  };

  for (const pb::Unit& u : snapshot.base.nearby.units) {
    record_cast(u);
  }
  if (snapshot.ext.encounter && snapshot.ext.encounter->boss) {
    record_cast(*snapshot.ext.encounter->boss);
  }
  if (snapshot.ext.target && snapshot.ext.target->target) {
    record_cast(*snapshot.ext.target->target);
  }

  // Track newly appearing living units, objects, and areas. On the very first
  // snapshot, seed `prev_entity_keys_` without firing `appears in last Ns` for
  // entities already present at login.
  std::flat_set<std::string> current_keys;
  auto record_present = [&](std::string_view guid, std::string_view name) {
    if (name.empty()) {
      return;
    }
    std::string key =
        !guid.empty() ? std::string(guid) : ("name:" + LowerKey(name));
    current_keys.insert(key);
    if (history_seeded_ && !prev_entity_keys_.contains(key)) {
      last_appears_[LowerKey(name)] = {now, std::string(guid),
                                       std::string(name)};
    }
  };

  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (!u.dead) {
      record_present(u.guid, u.name);
    }
  }
  if (snapshot.ext.encounter && snapshot.ext.encounter->boss &&
      !snapshot.ext.encounter->boss->dead) {
    record_present(snapshot.ext.encounter->boss->guid,
                   snapshot.ext.encounter->boss->name);
  }
  for (const pb::GameObject& o : snapshot.base.nearby.objects) {
    record_present(o.guid, o.name);
  }
  for (const pb::Area& a : snapshot.base.nearby.areas) {
    record_present(a.guid, AreaName(a));
  }
  prev_entity_keys_ = std::move(current_keys);
  history_seeded_ = true;
}

OrderEvalResult OrderEvaluator::DispatchStep(int order_idx,
                                             int step_idx,
                                             std::string it_guid,
                                             std::string it_name,
                                             bool preempted,
                                             base::TimeTicks now) {
  const Order& order = orders_[static_cast<size_t>(order_idx)];
  if (step_idx == 0) {
    if (OrderHasEvery(order)) {
      last_every_fired_[static_cast<size_t>(order_idx)] = now;
    } else {
      order_latched_[static_cast<size_t>(order_idx)] = true;
    }
  }
  const Act& act = order.steps[static_cast<size_t>(step_idx)];
  const bool is_always =
      std::holds_alternative<grammar::WhenAlways>(order.when.value);

  OrderEvalResult res;
  res.act = act;
  res.it_guid = it_guid;
  res.it_name = it_name;
  res.order_index = order_idx;
  res.step_index = step_idx;
  res.preempted = preempted;

  if (std::holds_alternative<grammar::ActWake>(act.value)) {
    ResetActive();
    res.covered = false;
    res.wake_reason = "standing order: " + FormatOrder(order);
    return res;
  }

  active_order_ = order_idx;
  active_step_ = step_idx;
  active_it_guid_ = std::move(it_guid);
  active_it_name_ = std::move(it_name);
  step_started_at_ = now;
  pending_command_id_ = 0;
  res.covered = !is_always;
  return res;
}

OrderEvalResult OrderEvaluator::OnSnapshot(const pb::Snapshot& snapshot,
                                           base::TimeTicks now) {
  UpdateHistory(snapshot, now);

  if (orders_.empty()) {
    return {};
  }

  // Check if the running step completed on this snapshot (separate from
  // `done_command_id` used by `PlanRunner`).
  bool step_advanced = false;
  if (active_order_ >= 0 && pending_command_id_ != 0 &&
      snapshot.done_act_command_id == pending_command_id_) {
    const Order& cur = orders_[static_cast<size_t>(active_order_)];
    const int next_step = active_step_ + 1;
    if (static_cast<size_t>(next_step) < cur.steps.size()) {
      active_step_ = next_step;
      pending_command_id_ = 0;
      step_advanced = true;
    } else {
      ResetActive();
    }
  }

  // If still waiting on a step that did not just finish, check its timeout.
  if (active_order_ >= 0 && !step_advanced) {
    OrderEvalResult timeout = CheckTimeout(now);
    if (!timeout.wake_reason.empty()) {
      return timeout;
    }
  }

  // Build snapshot view and bind history entries to live entities where
  // possible.
  SnapshotView view(snapshot, now, now);
  auto fallback_entity = [](const SeenEvent& ev) {
    Entity e;
    e.guid = ev.guid;
    e.name = ev.name;
    return e;
  };
  std::flat_map<std::string, std::pair<base::TimeTicks, Entity>> cast_entities;
  for (const auto& [k, ev] : last_casts_) {
    Entity e =
        view.FindByGuidOrName(ev.guid, ev.name).value_or(fallback_entity(ev));
    cast_entities.emplace(k, std::make_pair(ev.when, std::move(e)));
  }
  std::flat_map<std::string, std::pair<base::TimeTicks, Entity>>
      appear_entities;
  for (const auto& [k, ev] : last_appears_) {
    Entity e =
        view.FindByGuidOrName(ev.guid, ev.name).value_or(fallback_entity(ev));
    appear_entities.emplace(k, std::make_pair(ev.when, std::move(e)));
  }
  view.last_casts = &cast_entities;
  view.last_appears = &appear_entities;

  // Evaluate every order so edge latches re-arm whenever `when` goes false,
  // and select the highest-priority eligible order (strictly higher than
  // `active_order_` when an order is already running).
  int winner = -1;
  WhenMatch winner_match;
  bool active_matched = false;
  const int limit =
      active_order_ >= 0 ? active_order_ : static_cast<int>(orders_.size());
  for (int i = 0; i < static_cast<int>(orders_.size()); ++i) {
    const Order& order = orders_[static_cast<size_t>(i)];
    if (order.steps.empty()) {
      continue;
    }
    view.installed_at = order_installed_at_[static_cast<size_t>(i)];
    view.order_last_every = last_every_fired_[static_cast<size_t>(i)];
    WhenMatch match = EvalWhen(view, order.when);
    if (!match.matched) {
      order_latched_[static_cast<size_t>(i)] = false;
      continue;
    }
    if (i == active_order_) {
      active_matched = true;
    }
    const bool uses_every = OrderHasEvery(order);
    const bool latched = !uses_every && order_latched_[static_cast<size_t>(i)];
    if (winner < 0 && i < limit && !latched) {
      winner = i;
      winner_match = std::move(match);
    }
  }

  if (winner >= 0) {
    const bool preempted = active_order_ >= 0;
    if (preempted &&
        std::holds_alternative<grammar::WhenAlways>(
            orders_[static_cast<size_t>(active_order_)].when.value)) {
      // Allow a preempted `always` fallback to resume once the urgent order
      // finishes.
      order_latched_[static_cast<size_t>(active_order_)] = false;
    }
    std::string it_guid = winner_match.it ? winner_match.it->guid : "";
    std::string it_name = winner_match.it ? winner_match.it->name : "";
    OrderEvalResult res = DispatchStep(winner, 0, std::move(it_guid),
                                       std::move(it_name), preempted, now);
    if (res.act) {
      res.target_guid = ResolveActThingGuid(view, *res.act, winner_match.it);
    }
    return res;
  }

  // A `kite` runs until stopped: it ends when its order's `when` no longer
  // holds (the mob is far, the debuff is gone), not by itself.
  if (active_order_ >= 0 && !active_matched && !step_advanced) {
    const Order& cur = orders_[static_cast<size_t>(active_order_)];
    if (std::holds_alternative<grammar::ActKite>(
            cur.steps[static_cast<size_t>(active_step_)].value)) {
      OrderEvalResult res;
      res.order_index = active_order_;
      res.step_index = active_step_;
      res.kite_ended = true;
      ResetActive();
      return res;
    }
  }

  // No higher-priority order preempted. If our active order just advanced its
  // `then` chain, dispatch the next step now.
  if (active_order_ >= 0 && step_advanced) {
    OrderEvalResult res = DispatchStep(active_order_, active_step_,
                                       active_it_guid_, active_it_name_,
                                       /*preempted=*/false, now);
    if (res.act) {
      std::optional<Entity> it_ent =
          view.FindByGuidOrName(active_it_guid_, active_it_name_);
      res.target_guid = ResolveActThingGuid(view, *res.act, it_ent);
    }
    return res;
  }

  // Otherwise, the active step continues running.
  if (active_order_ >= 0) {
    const Order& cur = orders_[static_cast<size_t>(active_order_)];
    OrderEvalResult res;
    res.order_index = active_order_;
    res.step_index = active_step_;
    res.it_guid = active_it_guid_;
    res.it_name = active_it_name_;
    res.covered = !std::holds_alternative<grammar::WhenAlways>(cur.when.value);
    return res;
  }

  return {};
}

OrderEvalResult OrderEvaluator::OnStepDone(uint32_t command_id,
                                           base::TimeTicks now) {
  if (active_order_ < 0 || command_id == 0 ||
      command_id != pending_command_id_) {
    return {};
  }
  const Order& cur = orders_[static_cast<size_t>(active_order_)];
  const int next_step = active_step_ + 1;
  if (static_cast<size_t>(next_step) < cur.steps.size()) {
    return DispatchStep(active_order_, next_step, active_it_guid_,
                        active_it_name_, /*preempted=*/false, now);
  }
  ResetActive();
  return {};
}

OrderEvalResult OrderEvaluator::OnStepFailed(std::string_view error) {
  if (active_order_ < 0) {
    return {};
  }
  const int order_idx = active_order_;
  const int step_idx = active_step_;
  const std::string order_text =
      FormatOrder(orders_[static_cast<size_t>(order_idx)]);
  ResetActive();

  OrderEvalResult res;
  res.order_index = order_idx;
  res.step_index = step_idx;
  res.covered = false;
  res.wake_reason = "standing order failed (" + order_text + ")";
  if (!error.empty()) {
    res.wake_reason += ": ";
    res.wake_reason += error;
  }
  return res;
}

OrderEvalResult OrderEvaluator::CheckTimeout(base::TimeTicks now) {
  if (active_order_ < 0) {
    return {};
  }
  const Order& order = orders_[static_cast<size_t>(active_order_)];
  const Act& act = order.steps[static_cast<size_t>(active_step_)];
  const std::optional<base::TimeDelta> limit = TimeoutForAct(act);
  if (!limit || (now - step_started_at_) < *limit) {
    return {};
  }
  const int order_idx = active_order_;
  const int step_idx = active_step_;
  const std::string order_text = FormatOrder(order);
  ResetActive();

  OrderEvalResult res;
  res.order_index = order_idx;
  res.step_index = step_idx;
  res.covered = false;
  res.timed_out = true;
  res.wake_reason = "standing order timed out (" + order_text + ")";
  return res;
}

namespace {

bool IsMe(const Thing& thing) {
  const auto* who = std::get_if<grammar::ThingWho>(&thing.value);
  return who && who->who == grammar::Who::kMe;
}

std::optional<std::string_view> SpecificThingName(const Thing& thing) {
  if (const auto* named = std::get_if<grammar::ThingNamed>(&thing.value)) {
    return named->name;
  }
  if (const auto* nk = std::get_if<grammar::ThingNearest>(&thing.value)) {
    if (const auto* kn =
            std::get_if<grammar::NearestKindNamed>(&nk->kind.value)) {
      return kn->name;
    }
  }
  return std::nullopt;
}

}  // namespace

bool OrderCoversWake(const Order& order,
                     uint32_t kinds,
                     const std::vector<std::string>& attackers) {
  const auto* all = std::get_if<grammar::WhenAll>(&order.when.value);
  if (!all) {
    return false;  // `always` orders never suppress wake triggers.
  }
  // What this order's conditions are about, by the wake each one answers:
  // my health (`me hp<N`), my mana, something targeting me. Death, coming
  // back, loss of control, enemy players and anything else always wake.
  uint32_t covered = 0;
  for (const Condition& cond : all->conds) {
    if (cond.negated) {
      continue;
    }
    if (const auto* hp = std::get_if<grammar::CheckHp>(&cond.check.value);
        hp && IsMe(hp->a)) {
      covered |= kWakeHealth;
    }
    if (std::holds_alternative<grammar::CheckMana>(cond.check.value) ||
        std::holds_alternative<grammar::CheckManaPoints>(cond.check.value)) {
      covered |= kWakeMana;
    }
    if (const auto* targets =
            std::get_if<grammar::CheckTargets>(&cond.check.value);
        targets && IsMe(targets->b)) {
      if (const std::optional<std::string_view> specific =
              SpecificThingName(targets->a)) {
        // `X targets me` only covers attacks by X, never another attacker.
        const bool all_match = !attackers.empty() &&
                               std::all_of(attackers.begin(), attackers.end(),
                                           [&](const std::string& a) {
                                             return NameEq(a, *specific);
                                           });
        if (all_match) {
          covered |= kWakeAttacked | kWakeEnteredCombat;
        }
      } else {
        covered |= kWakeAttacked | kWakeEnteredCombat;
      }
    }
  }
  return kinds != 0 && (kinds & ~covered) == 0;
}

std::optional<pb::Command::Act> OrderActToCommand(
    const Act& act,
    std::string_view it_guid,
    std::string_view it_name,
    std::string_view target_guid) {
  auto fill_thing = [](const Thing& thing, pb::Command::Act* out,
                       bool target_slot) {
    std::string& dst_name = target_slot ? out->target_name : out->name;
    std::visit(
        [&](const auto& t) {
          using T = std::decay_t<decltype(t)>;
          if constexpr (std::is_same_v<T, grammar::ThingWho>) {
            switch (t.who) {
              case grammar::Who::kMe:
                out->thing = pb::Command::Act::ME;
                break;
              case grammar::Who::kTank:
                out->thing = pb::Command::Act::TANK;
                break;
              case grammar::Who::kOfftank:
                out->thing = pb::Command::Act::OFFTANK;
                break;
              case grammar::Who::kHealer:
                out->thing = pb::Command::Act::HEALER;
                break;
              case grammar::Who::kLeader:
                out->thing = pb::Command::Act::LEADER;
                break;
              case grammar::Who::kAnyone:
                out->thing = pb::Command::Act::ANYONE;
                break;
              case grammar::Who::kBoss:
                out->thing = pb::Command::Act::BOSS;
                break;
              case grammar::Who::kIt:
                out->thing = pb::Command::Act::IT;
                break;
            }
          } else if constexpr (std::is_same_v<T, grammar::ThingMyTarget>) {
            out->thing = pb::Command::Act::MY_TARGET;
          } else if constexpr (std::is_same_v<T, grammar::ThingFocus>) {
            out->thing = pb::Command::Act::FOCUS;
          } else if constexpr (std::is_same_v<T, grammar::ThingMarker>) {
            out->thing = pb::Command::Act::MARKER;
            out->marker = ToRaidMarker(t.icon);
          } else if constexpr (std::is_same_v<T, grammar::ThingNearest>) {
            std::visit(
                [&](const auto& k) {
                  using K = std::decay_t<decltype(k)>;
                  if constexpr (std::is_same_v<K, grammar::NearestKindAlly>) {
                    out->thing = pb::Command::Act::NEAREST_ALLY;
                  } else if constexpr (std::is_same_v<
                                           K, grammar::NearestKindPlayer>) {
                    out->thing = pb::Command::Act::NEAREST_PLAYER;
                  } else if constexpr (std::is_same_v<
                                           K, grammar::NearestKindEnemy>) {
                    out->thing = pb::Command::Act::NEAREST_ENEMY;
                  } else if constexpr (std::is_same_v<
                                           K, grammar::NearestKindNamed>) {
                    out->thing = pb::Command::Act::NEAREST_NAMED;
                    dst_name = k.name;
                  }
                },
                t.kind.value);
          } else if constexpr (std::is_same_v<T, grammar::ThingAllyWith>) {
            out->thing = pb::Command::Act::ALLY_WITH;
            dst_name = t.mech;
          } else if constexpr (std::is_same_v<T, grammar::ThingGroup>) {
            out->thing = pb::Command::Act::GROUP;
          } else if constexpr (std::is_same_v<T, grammar::ThingAllies>) {
            out->thing = pb::Command::Act::ALLIES;
          } else if constexpr (std::is_same_v<T, grammar::ThingNamed>) {
            out->thing = pb::Command::Act::NAMED;
            dst_name = t.name;
          }
        },
        thing.value);
  };

  pb::Command::Act out;
  out.it_guid = std::string(it_guid);
  out.it_name = std::string(it_name);

  bool supported = true;
  std::visit(
      [&](const auto& a) {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, grammar::ActTo>) {
          out.verb = pb::Command::Act::TO;
          std::visit(
              [&](const auto& p) {
                using P = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<P, grammar::PlaceBehind>) {
                  out.place = pb::Command::Act::BEHIND;
                  fill_thing(p.t, &out, /*target_slot=*/false);
                } else if constexpr (std::is_same_v<P, grammar::PlaceInFront>) {
                  out.place = pb::Command::Act::IN_FRONT;
                  fill_thing(p.t, &out, /*target_slot=*/false);
                } else if constexpr (std::is_same_v<P,
                                                    grammar::PlaceMaxRange>) {
                  out.place = pb::Command::Act::MAX_RANGE;
                  fill_thing(p.t, &out, /*target_slot=*/false);
                } else if constexpr (std::is_same_v<P, grammar::PlaceYdFrom>) {
                  out.place = pb::Command::Act::YD_FROM;
                  out.yd = static_cast<float>(p.yd);
                  fill_thing(p.t, &out, /*target_slot=*/false);
                } else if constexpr (std::is_same_v<P, grammar::PlaceAt>) {
                  out.place = pb::Command::Act::AT;
                  fill_thing(p.t, &out, /*target_slot=*/false);
                }
              },
              a.place.value);
        } else if constexpr (std::is_same_v<A, grammar::ActAway>) {
          out.verb = pb::Command::Act::AWAY;
          if (a.t) {
            fill_thing(*a.t, &out, /*target_slot=*/false);
          }
          if (a.yd) {
            out.yd = static_cast<float>(*a.yd);
          }
        } else if constexpr (std::is_same_v<A, grammar::ActTarget>) {
          out.verb = pb::Command::Act::TARGET;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActUse>) {
          out.verb = pb::Command::Act::USE;
          out.name = a.name;
          if (a.t) {
            fill_thing(*a.t, &out, /*target_slot=*/true);
          }
        } else if constexpr (std::is_same_v<A, grammar::ActStop>) {
          out.verb = pb::Command::Act::STOP;
          if (a.what) {
            switch (*a.what) {
              case grammar::Stop::kCasting:
                out.stop = pb::Command::Act::CASTING;
                break;
              case grammar::Stop::kAttacking:
                out.stop = pb::Command::Act::ATTACKING;
                break;
              case grammar::Stop::kMoving:
                out.stop = pb::Command::Act::MOVING;
                break;
            }
          }
        } else if constexpr (std::is_same_v<A, grammar::ActWake>) {
          out.verb = pb::Command::Act::WAKE;
        } else if constexpr (std::is_same_v<A, grammar::ActAvoid>) {
          out.verb = pb::Command::Act::AVOID;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActFace>) {
          out.verb = pb::Command::Act::FACE;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActFaceAway>) {
          out.verb = pb::Command::Act::FACE_AWAY;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActJump>) {
          out.verb = pb::Command::Act::JUMP;
        } else if constexpr (std::is_same_v<A, grammar::ActCancel>) {
          out.verb = pb::Command::Act::CANCEL;
          out.name = a.name;
        } else if constexpr (std::is_same_v<A, grammar::ActMark>) {
          out.verb = pb::Command::Act::MARK;
          fill_thing(a.t, &out, /*target_slot=*/false);
          out.marker = ToRaidMarker(a.icon);
        } else if constexpr (std::is_same_v<A, grammar::ActInteract>) {
          out.verb = pb::Command::Act::INTERACT;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActBoard>) {
          out.verb = pb::Command::Act::BOARD;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActLeave>) {
          out.verb = pb::Command::Act::LEAVE;
        } else if constexpr (std::is_same_v<A, grammar::ActEquip>) {
          out.verb = pb::Command::Act::EQUIP;
          out.name = a.name;
        } else if constexpr (std::is_same_v<A, grammar::ActPetAttack>) {
          out.verb = pb::Command::Act::PET_ATTACK;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else if constexpr (std::is_same_v<A, grammar::ActPet>) {
          out.verb = pb::Command::Act::PET;
          switch (a.cmd) {
            case grammar::PetCmd::kStay:
              out.pet_cmd = pb::Command::Act::STAY;
              break;
            case grammar::PetCmd::kFollow:
              out.pet_cmd = pb::Command::Act::FOLLOW;
              break;
            case grammar::PetCmd::kPassive:
              out.pet_cmd = pb::Command::Act::PASSIVE;
              break;
            case grammar::PetCmd::kDefensive:
              out.pet_cmd = pb::Command::Act::DEFENSIVE;
              break;
          }
        } else if constexpr (std::is_same_v<A, grammar::ActKite>) {
          out.verb = pb::Command::Act::KITE;
          fill_thing(a.t, &out, /*target_slot=*/false);
        } else {
          supported = false;
        }
      },
      act.value);

  if (!supported) {
    return std::nullopt;
  }
  if (!target_guid.empty() &&
      out.thing != pb::Command::Act::THING_UNSPECIFIED &&
      out.thing != pb::Command::Act::ME && out.thing != pb::Command::Act::IT &&
      out.thing != pb::Command::Act::FOCUS) {
    out.it_guid = std::string(target_guid);
  }
  return out;
}

}  // namespace harness
