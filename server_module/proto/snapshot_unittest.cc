// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The snapshot BEVE and JSON codecs round-trip through the bridge's frame
// codec: what the module serializes is what the harness parses.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/frame_codec.h"
#include "proto/snapshot.h"

namespace llm_brain {
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

}  // namespace
}  // namespace llm_brain
