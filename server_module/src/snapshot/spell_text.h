// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_SPELL_TEXT_H_
#define MOD_LLM_BRAIN_SNAPSHOT_SPELL_TEXT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace llm_brain {

// Where a spell description's numbers come from (the module: the spell's
// effects and duration, tooltips.cc; tests: a fake).
class SpellValueSource {
 public:
  virtual ~SpellValueSource() = default;
  // The value of `code` (s, o, t, m, a, d, h, n, u, x, ...) for effect `index`
  // (1-3; 0 when the token has none) of spell `spell_id` (0: the described
  // spell itself), as the tooltip prints it ("61", "18 sec"); nullopt if
  // unknown.
  virtual std::optional<std::string> Value(uint32_t spell_id,
                                           char code,
                                           int index) const = 0;
};

// Fills in a spell description's tokens the way the client does for the
// common ones: `$s1`, `$o1`, `$t1`, `$d`, `$a1`, `$h`, `$n`, `$u`, `$x1`,
// `$m1` (with an optional other spell's id: `$12345s1`), `$z` (the home
// location), `$lone:many;` (after the number before it) and `$ghis:her;` (the
// first form). Anything
// else (`${...}` expressions, `$/1000;s1` divisions, unknown values) is left
// as written.
std::string FormatSpellText(std::string_view text,
                            const SpellValueSource& values);

// Fills in a quest text's client tokens: `$B`/`$b` (paragraph break -> space),
// `$N`/`$n` (name), `$C`/`$c` (class), `$R`/`$r` (race), and `$Gmale:female;` /
// `$gmale:female;`.
std::string FormatQuestText(std::string_view text,
                            std::string_view name,
                            std::string_view class_name,
                            std::string_view race_name,
                            bool female);

// Spell.dbc (3.3.5a WDBC) column of the description in the given locale:
// field 170 is enUS, then one per locale (DBCStructure.h: not loaded by
// AzerothCore, "not used" by the server).
inline constexpr uint32_t kSpellDbcDescriptionField = 170;

// The descriptions of `wanted` spells from the bytes of Spell.dbc, in locale
// `locale` (0: enUS); spells without one are left out. Empty if `dbc` isn't a
// WDBC file with that many fields.
std::unordered_map<uint32_t, std::string> ParseSpellDescriptions(
    std::string_view dbc,
    const std::vector<uint32_t>& wanted,
    uint32_t locale);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_SPELL_TEXT_H_
