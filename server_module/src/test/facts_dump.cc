// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/facts_dump.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CreatureData.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "MapMgr.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "options/option_executor.h"
#include "options/option_facts.h"
#include "options/options.h"
#include "options/position_facts.h"
#include "proto/snapshot.h"
#include "snapshot/perception.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

std::optional<std::string> ReadFileBytes(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return std::nullopt;
  }
  std::string out;
  char buf[1 << 16];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  std::fclose(f);
  return out;
}

bool WriteFileBytes(const std::string& path, std::string_view bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

std::optional<uint32_t> MapIdByName(std::string_view name) {
  for (uint32_t i = 0; i < sMapStore.GetNumRows(); ++i) {
    if (const MapEntry* entry = sMapStore.LookupEntry(i)) {
      const char* map_name = entry->name[sWorld->GetDefaultDbcLocale()];
      if (map_name && name == map_name) {
        return i;
      }
    }
  }
  return std::nullopt;
}

uint8_t ClassIdByName(std::string_view name) {
  for (uint8_t id = 1; id < MAX_CLASSES; ++id) {
    if (name == ClassName(id)) {
      return id;
    }
  }
  return 0;
}

bool DumpOne(const WorldPlaces& world,
             const std::vector<std::string>& q,
             std::string* error) {
  const std::optional<std::string> text = ReadFileBytes(q[0]);
  pb::Snapshot snapshot;
  if (!text || !pb::ReadJson(*text, &snapshot, error)) {
    *error = "can't read " + q[0] + (error->empty() ? "" : ": " + *error);
    return false;
  }
  const pb::Base& base = snapshot.base;
  const std::optional<uint32_t> map_id = MapIdByName(base.where.map);
  if (!map_id) {
    *error = q[0] + ": unknown map \"" + base.where.map + "\"";
    return false;
  }
  Viewpoint view;
  view.map_id = *map_id;
  view.pos = base.where.position;
  sMapMgr->GetZoneAndAreaId(view.phase_mask, view.zone_id, view.area_id,
                            view.map_id, view.pos.x, view.pos.y, view.pos.z);
  view.level = static_cast<uint8_t>(base.self.unit.level.value_or(1));
  view.class_id =
      base.self.unit.player ? ClassIdByName(base.self.unit.player->class_) : 0;
  view.alliance = q[2] == "ALLIANCE";

  OptionFacts facts;
  for (const pb::Unit& u : base.nearby.units) {
    if (u.kind != pb::Unit::NPC || u.dead || u.reaction != pb::Unit::FRIENDLY) {
      continue;
    }
    const ObjectGuid guid = ParseSnapshotGuid(u.guid);
    if (const CreatureTemplate* tmpl =
            guid ? sObjectMgr->GetCreatureTemplate(guid.GetEntry()) : nullptr;
        tmpl && (tmpl->npcflag & kTalkNpcFlags) != 0) {
      facts.npcs.push_back({u.guid, tmpl->npcflag});
    }
  }
  facts.has_food = q[3] == "1";
  facts.has_drink = q[4] == "1";
  base::StringToUint(q[5], &facts.herbalism_skill);
  base::StringToUint(q[6], &facts.mining_skill);

  const bool alive = base.self.state != pb::Self::DEAD_BODY &&
                     base.self.state != pb::Self::GHOST;
  if (alive && !base.self.in_combat) {
    pb::Minimap minimap;  // The fixture's own screen is not changed.
    AddPositionFacts(ComputePositionFacts(world, view, nullptr), view.pos,
                     snapshot.ext.quest_tracker, &facts, &minimap);
  }
  if (!WriteFileBytes(q[1], pb::WriteJson(facts, /*pretty=*/true) + "\n")) {
    *error = "can't write " + q[1];
    return false;
  }
  return true;
}

}  // namespace

int DumpFixtureFacts(const WorldPlaces& world,
                     const std::string& queries_path,
                     std::string* error) {
  const std::optional<std::string> text = ReadFileBytes(queries_path);
  if (!text) {
    *error = "can't read " + queries_path;
    return -1;
  }
  int written = 0;
  for (const std::string& line : base::SplitString(
           *text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const std::vector<std::string> q = base::SplitString(
        line, "\t", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    if (q.size() != 7) {
      *error = "bad query line (want 7 fields): " + line;
      return -1;
    }
    if (!DumpOne(world, q, error)) {
      return -1;
    }
    ++written;
  }
  return written;
}

}  // namespace llm_brain
