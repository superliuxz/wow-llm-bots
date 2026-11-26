// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The snapshot BEVE and JSON codecs round-trip through the bridge's frame
// codec: what the module serializes is what the harness parses.

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/frame_codec.h"
#include "snapshot/snapshot.h"

namespace wlb::snapshot {
namespace {

namespace pb = wlb::snapshot;

pb::ModuleMessage MakeSnapshotMessage() {
  pb::ModuleMessage message;
  pb::Snapshot& snapshot = message.snapshot.emplace();
  snapshot.bot = "Dezlu";
  snapshot.seq = 412;
  snapshot.time_ms = 1794729600000;

  pb::Self& self = snapshot.base.self;
  self.unit.name = "Dezlu";
  self.unit.level = 5;
  self.unit.player.emplace().class_ = "Priest";
  self.health = 96;
  self.health_max = 118;
  self.state = pb::Self::ALIVE;

  pb::Unit& boar = snapshot.base.nearby.units.emplace_back();
  boar.name = "Mottled Boar";
  boar.reaction = pb::Unit::HOSTILE;
  boar.rel.dist_yd = 28.5f;
  boar.rel.dir = pb::S;

  snapshot.events.emplace_back().chat.emplace().text = "hi\nthere";
  pb::Option& option = snapshot.options.emplace_back();
  option.text = "Buy a guild charter named ___";
  option.blanks.emplace_back().kind = pb::Blank::NEW_NAME;
  return message;
}

TEST(SnapshotProtoTest, RoundTripsThroughFrames) {
  const pb::ModuleMessage sent = MakeSnapshotMessage();
  std::string stream;
  ASSERT_TRUE(net::AppendFrame(pb::WriteBeve(sent), &stream));

  net::FrameDecoder decoder;
  const std::vector<std::string> frames = decoder.Append(stream);
  ASSERT_EQ(frames.size(), 1u);
  pb::ModuleMessage received;
  ASSERT_TRUE(pb::ReadBeve(frames[0], &received));

  ASSERT_TRUE(received.snapshot.has_value());
  const pb::Snapshot& snapshot = *received.snapshot;
  EXPECT_EQ(snapshot.bot, "Dezlu");
  EXPECT_EQ(snapshot.time_ms, 1794729600000u);
  ASSERT_TRUE(snapshot.base.self.unit.player.has_value());
  EXPECT_EQ(snapshot.base.self.unit.player->class_, "Priest");
  EXPECT_EQ(snapshot.base.nearby.units[0].rel.dir, pb::S);
  ASSERT_TRUE(snapshot.events[0].chat.has_value());
  EXPECT_EQ(snapshot.events[0].chat->text, "hi\nthere");
  EXPECT_EQ(snapshot.options[0].blanks[0].kind, pb::Blank::NEW_NAME);
  EXPECT_EQ(received, sent);
}

TEST(SnapshotProtoTest, DefaultsAreOmittedInJsonAndCompactInBeve) {
  // Default fields (false / 0 / "" / *_UNSPECIFIED / empty substructs) are
  // omitted completely in JSON, and scalar/null defaults cost 0 bytes in BEVE.
  pb::Snapshot empty;
  EXPECT_EQ(pb::WriteJson(empty), "{}");
  pb::Snapshot round_trip;
  ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(empty), &round_trip));
  EXPECT_EQ(round_trip, empty);
  EXPECT_LE(pb::WriteBeve(pb::Command{}).size(), 4u);
}

TEST(SnapshotProtoTest, CommandCarriesBlankFills) {
  pb::Command command;
  command.bot = "Zul'jinx";
  auto& choose = command.choose.emplace();
  choose.option = 4;
  choose.fills.push_back("Totem Pole");

  pb::Command parsed;
  ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(command), &parsed));
  ASSERT_TRUE(parsed.choose.has_value());
  EXPECT_EQ(parsed.choose->option, 4u);
  ASSERT_EQ(parsed.choose->fills.size(), 1u);
  EXPECT_EQ(parsed.choose->fills[0], "Totem Pole");
}

// A set optional holding 0 or false is not the same as an unset one (bags with
// 0 free slots are full; unset is unknown): both codecs must keep it.
TEST(SnapshotProtoTest, ZeroAndFalseOptionalsSurvive) {
  pb::Snapshot snap;
  snap.base.self.indicators.bag_free_slots = 0;
  snap.base.nearby.units.emplace_back().rel.in_los = false;
  snap.done_command_id = 7;

  pb::Snapshot from_beve;
  ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(snap), &from_beve));
  EXPECT_EQ(from_beve, snap);
  ASSERT_TRUE(from_beve.base.self.indicators.bag_free_slots.has_value());
  ASSERT_TRUE(from_beve.base.nearby.units[0].rel.in_los.has_value());

  pb::Snapshot from_json;
  ASSERT_TRUE(pb::ReadJson(pb::WriteJson(snap), &from_json));
  EXPECT_EQ(from_json, snap);
}

// Whatever arrives on the socket, reading it fails cleanly or succeeds: never a
// crash (the sanitizer builds run this too).
TEST(SnapshotProtoTest, HelloNamesThisBuildAndRoundTrips) {
  const std::string& hash = pb::SchemaHash();
  ASSERT_EQ(hash.size(), 16u);
  EXPECT_EQ(hash.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_EQ(&pb::SchemaHash(), &hash);  // Computed once.

  pb::ModuleMessage sent;
  sent.version = pb::kWireVersion;
  sent.hello = pb::MakeHello("module");
  pb::ModuleMessage received;
  ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(sent), &received));
  EXPECT_EQ(received, sent);
  EXPECT_FALSE(received.snapshot);
  EXPECT_FALSE(received.event);
  EXPECT_EQ(pb::HelloMismatch(*received.hello), "");

  pb::Command cmd;
  cmd.version = pb::kWireVersion;
  cmd.hello = pb::MakeHello("harness");
  pb::Command cmd_back;
  ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(cmd), &cmd_back));
  EXPECT_EQ(cmd_back, cmd);
  EXPECT_TRUE(cmd_back.bot.empty());
}

TEST(SnapshotProtoTest, HelloMismatchNamesWhatDiffers) {
  pb::Hello other_version = pb::MakeHello("harness");
  other_version.wire_version = pb::kWireVersion + 1;
  const std::string version_error = pb::HelloMismatch(other_version);
  EXPECT_NE(version_error.find("harness speaks wire version"),
            std::string::npos)
      << version_error;
  EXPECT_NE(version_error.find("rebuild"), std::string::npos);

  pb::Hello other_schema = pb::MakeHello("");
  other_schema.schema_hash = "0000000000000000";
  const std::string schema_error = pb::HelloMismatch(other_schema);
  EXPECT_NE(schema_error.find("peer has wire schema 0000000000000000"),
            std::string::npos)
      << schema_error;

  // An empty hello (a peer that doesn't know the fields) never matches.
  EXPECT_NE(pb::HelloMismatch(pb::Hello{}), "");
}

// A reader of an older build skips the hello it doesn't know: a hello-only
// message reads as one with neither snapshot nor event.
TEST(SnapshotProtoTest, HelloOnlyMessageCarriesNothingElse) {
  pb::ModuleMessage hello;
  hello.version = pb::kWireVersion;
  hello.hello = pb::MakeHello("module");
  const std::string json = pb::WriteJson(hello);
  EXPECT_NE(json.find("\"hello\""), std::string::npos) << json;
  EXPECT_EQ(json.find("snapshot"), std::string::npos) << json;
}

TEST(SnapshotProtoTest, CorruptFramesNeverCrash) {
  const std::string good = pb::WriteBeve(MakeSnapshotMessage());
  uint32_t state = 12345;
  const auto next = [&state] {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  };
  for (int i = 0; i < 4000; ++i) {
    std::string bad = good;
    const int flips = 1 + static_cast<int>(next() % 4);
    for (int f = 0; f < flips; ++f) {
      bad[next() % bad.size()] = static_cast<char>(next());
    }
    if (i % 3 == 0) {
      bad.resize(next() % bad.size());
    }
    pb::ModuleMessage msg;
    (void)pb::ReadBeve(bad, &msg);
  }
}

// A corrupt frame claiming a huge array is refused, not allocated.
TEST(SnapshotProtoTest, OversizedArraysAreRefused) {
  pb::Snapshot big;
  big.options.resize(20000);
  for (pb::Option& o : big.options) {
    o.text = "x";
  }
  pb::Snapshot parsed;
  EXPECT_FALSE(pb::ReadBeve(pb::WriteBeve(big), &parsed));
}

// An enum value with no name can't be written as JSON: nothing, not half a
// document.
TEST(SnapshotProtoTest, WriteJsonOfAnUnnamedEnumValueIsEmpty) {
  pb::Snapshot snap;
  pb::ChatEvent& chat = snap.events.emplace_back().chat.emplace();
  chat.channel = static_cast<pb::ChatChannel>(250);
  chat.text = "hi";
  EXPECT_EQ(pb::WriteJson(snap), "");
}

// The visibility walker reaches every field by reflection (review of
// f40b0b6, finding 17): a hover-only field deep in the nearby units is caught
// outside a tooltip and allowed in one; a tooltip is for something hovered.
TEST(SnapshotVisibilityTest,
     HoverOnlyFieldsAnywhereAndTooltipsForWhatIsHovered) {
  pb::Snapshot snap;
  pb::Unit& unit = snap.base.nearby.units.emplace_back();
  unit.name = "Gatda";
  unit.player.emplace().race = "Orc";
  std::vector<std::string> errors = ValidateSnapshotVisibility(snap);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0],
            "$.base.nearby.units[0].player.race: hover-only field outside "
            "base.tooltips");

  snap.base.nearby.units[0].player->race.clear();
  pb::Tooltipped& tip = snap.base.tooltips.emplace_back();
  tip.ref.emplace().kind = pb::HoverRef::ITEM;
  tip.ref->id = "4540";
  pb::Item& item = tip.item.emplace();
  item.name = "Tough Hunk of Bread";
  item.vendor_value = 1;  // Hover-only, inside the tooltip: fine.
  errors = ValidateSnapshotVisibility(snap);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0],
            "$.base.tooltips[0]: not for anything in base.ui.hovering");
  snap.base.ui.hovering.push_back(*tip.ref);
  EXPECT_TRUE(ValidateSnapshotVisibility(snap).empty());
}

TEST(SnapshotProtoTest, SelfSpellMissRoundTripsBeveAndJson) {
  pb::Snapshot snap;
  snap.bot = "Vesper";
  pb::Self::SpellMiss& miss = snap.base.self.spell_miss.emplace();
  miss.spell = "Sap";
  miss.target = "Defias Cutpurse";
  miss.reason = "dodge";
  miss.seq = 3;

  const std::string beve = pb::WriteBeve(snap);
  ASSERT_FALSE(beve.empty());
  pb::Snapshot from_beve;
  ASSERT_TRUE(pb::ReadBeve(beve, &from_beve));
  EXPECT_EQ(from_beve, snap);

  const std::string json = pb::WriteJson(snap);
  ASSERT_FALSE(json.empty());
  pb::Snapshot from_json;
  std::string err;
  ASSERT_TRUE(pb::ReadJson(json, &from_json, &err)) << err;
  EXPECT_EQ(from_json, snap);
  EXPECT_TRUE(ValidateSnapshotVisibility(from_json).empty());
}

}  // namespace
}  // namespace wlb::snapshot
