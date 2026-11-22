// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/world_util.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Bag.h"
#include "Common.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "World.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

std::string AreaName(uint32_t area_id) {
  const AreaTableEntry* area = sAreaTableStore.LookupEntry(area_id);
  if (!area) {
    return "";
  }
  const char* name = area->area_name[sWorld->GetDefaultDbcLocale()];
  return name ? name : "";
}

std::string GuidString(uint64_t raw_guid) {
  return Acore::StringFormat("0x{:016X}", raw_guid);
}

wlb::snapshot::RaidMarker RaidMarkerOf(const Player* bot, uint64_t raw_guid) {
  const Group* grp = bot ? bot->GetGroup() : nullptr;
  if (!grp || raw_guid == 0) {
    return wlb::snapshot::RaidMarker::RAID_MARKER_UNSPECIFIED;
  }
  static constexpr wlb::snapshot::RaidMarker kIcons[TARGETICONCOUNT] = {
      wlb::snapshot::RaidMarker::STAR,    wlb::snapshot::RaidMarker::CIRCLE,
      wlb::snapshot::RaidMarker::DIAMOND, wlb::snapshot::RaidMarker::TRIANGLE,
      wlb::snapshot::RaidMarker::MOON,    wlb::snapshot::RaidMarker::SQUARE,
      wlb::snapshot::RaidMarker::CROSS,   wlb::snapshot::RaidMarker::SKULL,
  };
  for (uint8_t i = 0; i < TARGETICONCOUNT; ++i) {
    if (grp->GetTargetIcon(i).GetRawValue() == raw_guid) {
      return kIcons[i];
    }
  }
  return wlb::snapshot::RaidMarker::RAID_MARKER_UNSPECIFIED;
}

bool IsGeneralBag(const Bag* bag) {
  const ItemTemplate* p = bag ? bag->GetTemplate() : nullptr;
  return p && p->Class == ITEM_CLASS_CONTAINER &&
         p->SubClass == ITEM_SUBCLASS_CONTAINER;
}

std::vector<BagSlotItem> BagItems(Player* bot) {
  std::vector<BagSlotItem> out;
  for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END;
       ++slot) {
    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
      out.push_back({item, INVENTORY_SLOT_BAG_0, slot, true});
    }
  }
  for (uint8_t bag_pos = INVENTORY_SLOT_BAG_START;
       bag_pos < INVENTORY_SLOT_BAG_END; ++bag_pos) {
    Bag* bag = bot->GetBagByPos(bag_pos);
    if (!bag) {
      continue;
    }
    const bool general = IsGeneralBag(bag);
    for (uint32_t slot = 0; slot < bag->GetBagSize(); ++slot) {
      if (Item* item = bot->GetItemByPos(bag_pos, static_cast<uint8_t>(slot))) {
        out.push_back({item, bag_pos, static_cast<uint8_t>(slot), general});
      }
    }
  }
  return out;
}

wlb::snapshot::Self::Control ReadControlState(const Player* bot) {
  wlb::snapshot::Self::Control control;
  control.stunned = bot->HasUnitState(UNIT_STATE_STUNNED) ||
                    bot->HasUnitFlag(UNIT_FLAG_STUNNED);
  control.feared =
      bot->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
      bot->HasUnitFlag(UNIT_FLAG_FLEEING | UNIT_FLAG_CONFUSED);
  control.charmed =
      bot->HasUnitState(UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED) ||
      !bot->GetCharmerGUID().IsEmpty();
  control.silenced = bot->HasUnitFlag(UNIT_FLAG_SILENCED);
  control.rooted = bot->HasUnitState(UNIT_STATE_ROOT);
  control.pacified = bot->HasUnitFlag(UNIT_FLAG_PACIFIED);
  control.disarmed = bot->HasUnitFlag(UNIT_FLAG_DISARMED);
  return control;
}

uint8_t ControlMask(const Player* bot) {
  const wlb::snapshot::Self::Control c = ReadControlState(bot);
  return static_cast<uint8_t>(
      (c.stunned ? 1u << 0 : 0u) | (c.feared ? 1u << 1 : 0u) |
      (c.charmed ? 1u << 2 : 0u) | (c.silenced ? 1u << 3 : 0u) |
      (c.rooted ? 1u << 4 : 0u) | (c.pacified ? 1u << 5 : 0u) |
      (c.disarmed ? 1u << 6 : 0u));
}

namespace {

// Returns the small deduplicated list of units watched for reactive snapshots:
// the bot itself, its selected target, and its attackers.
std::vector<const Unit*> WatchedReactiveUnits(Player* bot) {
  std::vector<const Unit*> units;
  auto add = [&](const Unit* u) {
    if (u && u->IsInWorld() &&
        std::find(units.begin(), units.end(), u) == units.end()) {
      units.push_back(u);
    }
  };
  add(bot);
  add(bot->GetSelectedUnit());
  for (const Unit* attacker : bot->getAttackers()) {
    add(attacker);
  }
  return units;
}

template <typename CastFn, typename AuraFn>
void ForEachReactiveEntry(const Unit* u, CastFn&& on_cast, AuraFn&& on_aura) {
  const uint64_t guid = u->GetGUID().GetRawValue();
  for (CurrentSpellTypes type :
       {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL}) {
    if (const Spell* spell = u->GetCurrentSpell(type)) {
      if (const SpellInfo* info = spell->GetSpellInfo()) {
        on_cast(guid, info->Id);
      }
    }
  }
  for (const auto& [spell_id, aura] : u->GetAppliedAuras()) {
    (void)aura;
    on_aura(guid, spell_id);
  }
}

}  // namespace

ReactiveSnapshotState CollectReactiveState(Player* bot) {
  ReactiveSnapshotState state;
  for (const Unit* u : WatchedReactiveUnits(bot)) {
    ForEachReactiveEntry(
        u,
        [&](uint64_t guid, uint32_t spell_id) {
          state.casts.emplace_back(guid, spell_id);
        },
        [&](uint64_t guid, uint32_t spell_id) {
          state.auras.emplace_back(guid, spell_id);
        });
  }
  std::sort(state.casts.begin(), state.casts.end());
  state.casts.erase(std::unique(state.casts.begin(), state.casts.end()),
                    state.casts.end());
  std::sort(state.auras.begin(), state.auras.end());
  state.auras.erase(std::unique(state.auras.begin(), state.auras.end()),
                    state.auras.end());
  return state;
}

bool HasNewCastOrAura(Player* bot, const ReactiveSnapshotState& last) {
  bool found = false;
  for (const Unit* u : WatchedReactiveUnits(bot)) {
    ForEachReactiveEntry(
        u,
        [&](uint64_t guid, uint32_t spell_id) {
          if (!found &&
              !std::binary_search(last.casts.begin(), last.casts.end(),
                                  std::make_pair(guid, spell_id))) {
            found = true;
          }
        },
        [&](uint64_t guid, uint32_t spell_id) {
          if (!found &&
              !std::binary_search(last.auras.begin(), last.auras.end(),
                                  std::make_pair(guid, spell_id))) {
            found = true;
          }
        });
    if (found) {
      return true;
    }
  }
  return false;
}

std::string_view SpellLocalizedName(const SpellInfo* info) {
  if (!info) {
    return {};
  }
  const char* name = info->SpellName[sWorld->GetDefaultDbcLocale()];
  if (!name || name[0] == '\0') {
    name = info->SpellName[LOCALE_enUS];
  }
  return name ? std::string_view(name) : std::string_view();
}

float MaxSpellRadiusYd(const SpellInfo* info, Unit* caster, Spell* spell) {
  if (!info) {
    return 0.0f;
  }
  float max_r = 0.0f;
  for (uint8_t eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
    const float r = info->Effects[eff].CalcRadius(caster, spell);
    if (r > max_r) {
      max_r = r;
    }
  }
  return max_r;
}

std::optional<TriggerHazardInfo> InspectTriggerHazard(Creature* trigger) {
  if (!trigger || !trigger->IsAlive()) {
    return std::nullopt;
  }
  if (Spell* cast = trigger->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      cast && cast->getState() == SPELL_STATE_PREPARING) {
    const SpellInfo* info = cast->GetSpellInfo();
    const std::string_view name = SpellLocalizedName(info);
    if (info && !name.empty()) {
      TriggerHazardInfo h;
      h.name = std::string(name);
      h.radius_yd = MaxSpellRadiusYd(info, trigger, cast);
      h.lands_in_s =
          static_cast<float>(std::max(0, cast->GetCastTimeRemaining())) /
          1000.0f;
      h.spell_id = info->Id;
      h.is_preparing_cast = true;
      return h;
    }
  }
  if (Spell* ch = trigger->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
      ch && ch->getState() != SPELL_STATE_FINISHED) {
    const SpellInfo* info = ch->GetSpellInfo();
    const std::string_view name = SpellLocalizedName(info);
    if (info && !name.empty()) {
      TriggerHazardInfo h;
      h.name = std::string(name);
      h.radius_yd = MaxSpellRadiusYd(info, trigger, ch);
      h.remaining_s =
          static_cast<float>(std::max(0, ch->GetCastTimeRemaining())) / 1000.0f;
      h.spell_id = info->Id;
      return h;
    }
  }
  auto has_visual = [](const SpellInfo* s) {
    return s && (s->SpellVisual[0] != 0 || s->SpellVisual[1] != 0);
  };
  // Prefer periodic-trigger / non-zero-radius auras over visual-only dummy
  // auras (GetAppliedAuras is ordered by ascending spell_id).
  std::optional<TriggerHazardInfo> best;
  int best_rank = -1;
  for (const auto& [spell_id, aura_app] : trigger->GetAppliedAuras()) {
    (void)spell_id;
    if (!aura_app || !aura_app->GetBase()) {
      continue;
    }
    const SpellInfo* ainfo = aura_app->GetBase()->GetSpellInfo();
    if (!ainfo || ainfo->IsPassive()) {
      continue;
    }
    const SpellInfo* triggered = nullptr;
    for (uint8_t eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
      const SpellEffectInfo& e = ainfo->Effects[eff];
      if ((e.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL ||
           e.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE) &&
          e.TriggerSpell != 0) {
        if (const SpellInfo* t = sSpellMgr->GetSpellInfo(e.TriggerSpell)) {
          triggered = t;
          break;
        }
      }
    }
    if (!has_visual(ainfo) && !has_visual(triggered)) {
      continue;
    }
    const SpellInfo* effective =
        (triggered && !SpellLocalizedName(triggered).empty()) ? triggered
                                                              : ainfo;
    const std::string_view name = SpellLocalizedName(effective);
    if (name.empty()) {
      continue;
    }
    float radius = MaxSpellRadiusYd(effective, trigger);
    if (radius <= 0.0f && effective != ainfo) {
      radius = MaxSpellRadiusYd(ainfo, trigger);
    }
    const int rank = (triggered ? 2 : 0) + (radius > 0.0f ? 1 : 0);
    if (rank <= best_rank) {
      continue;
    }
    TriggerHazardInfo h;
    h.name = std::string(name);
    h.radius_yd = radius;
    if (aura_app->GetBase()->GetDuration() > 0) {
      h.remaining_s =
          static_cast<float>(aura_app->GetBase()->GetDuration()) / 1000.0f;
    }
    h.spell_id = effective->Id;
    best = std::move(h);
    best_rank = rank;
  }
  return best;
}

}  // namespace llm_brain
