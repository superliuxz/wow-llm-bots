// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Writes a bot's snapshot as the compact plain text the brain reads (its
// "screen"). The snapshot stays complete; this only decides how it is
// written for an LLM, which reads text tokens: one short line per unit or
// object in plain words, defaults left out (full health, in line of sight,
// idle ...), units that look the same as one line, far units that only show
// a model summarised by count. Nothing a bot could act on is dropped, except
// raw coordinates and GUIDs (distance and compass direction stay).
//
// The entries the bot can act on carry their option number, so the screen is
// also the menu the brain picks from (docs/specs/triggers.md). Until the
// module sends a real options list, the options are derived from the entries:
// attack, talk, loot, gather, use, plus a few that are always there.
//
// Ported from a retired Python prototype (prompt_render.py), and tested
// against its output on real snapshots (testdata/screens). Not rendered yet,
// because the module does not fill them yet: auras, services, group, boss, pet
// and vehicle parts of a unit, a unit's cast, `doing`, `ui`, `tooltips`, and
// every extension, event and option the snapshot can carry.

#ifndef HARNESS_SCREEN_SCREEN_RENDERER_H_
#define HARNESS_SCREEN_SCREEN_RENDERER_H_

#include <string>
#include <string_view>
#include <vector>

#include "snapshot/snapshot.h"

namespace harness {

// What a numbered option does.
enum class OptionKind {
  kAttack,
  kTalk,
  kLoot,
  kGather,
  kUse,
  kKeep,     // Keep doing what I'm doing.
  kExplore,  // Explore somewhere new.
  kRest,
  kTown,  // Go to the nearest town.
  kInvite,
  kAcceptInvite,
  kDeclineInvite,
  kLeaveGroup,
};

// "attack", "talk", "loot", ...
std::string_view OptionKindName(OptionKind kind);

// What one numbered option on the screen targets.
struct ScreenOption {
  OptionKind kind = OptionKind::kKeep;
  std::string guid;  // Empty for non-target options ("Keep ...").
  std::string name;  // Target name ("Mottled Boar"), for re-resolving repeats.

  bool operator==(const ScreenOption&) const = default;
};

// Something on screen a `hover:` line may name: an item in an open panel or a
// quest in the tracker or log (docs/specs/options.md).
struct Hoverable {
  std::string name;
  wlb::snapshot::HoverRef ref;

  bool operator==(const Hoverable&) const = default;
};

struct Screen {
  std::string text;
  // options[i] is what option number i + 1 does.
  std::vector<OptionKind> options;
  // Same length as `options`: target guid and name for each 1-based option.
  std::vector<ScreenOption> details;
  // True when option numbers came from `snapshot.options` (safe to send
  // Command.Choose); false when derived locally from an offline snapshot.
  bool has_module_options = false;
  // What can be pointed at, one per name (first one wins).
  std::vector<Hoverable> hoverables;
  // What is already on the screen: the open panels and the names of the
  // tooltips shown (an `open:` / `hover:` of these shows nothing new).
  std::vector<wlb::snapshot::Panel> open_panels;
  std::vector<std::string> shown_tooltips;
};

// The lines of one open panel (bags, character, quest log), as on the screen
// ("bags (3 of 16 free): ..."); empty if the snapshot doesn't carry it. Also
// what the harness remembers once the panel is closed.
std::vector<std::string> PanelLines(const wlb::snapshot::Snapshot& snapshot,
                                    wlb::snapshot::Panel panel);

// The name a reply uses for `panel` ("bags", "quest log"; plan.wlb's `Panel`),
// and back; PANEL_UNSPECIFIED for an unknown name.
std::string_view PanelName(wlb::snapshot::Panel panel);
wlb::snapshot::Panel PanelFromName(std::string_view name);

Screen RenderScreen(const wlb::snapshot::Snapshot& snapshot);

}  // namespace harness

#endif  // HARNESS_SCREEN_SCREEN_RENDERER_H_
