// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The generated snapshot code (lite runtime) round-trips through the bridge's
// frame codec: what the module serializes is what the harness parses.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/frame_codec.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {
namespace {

wlb::snapshot::ModuleMessage MakeSnapshotMessage() {
  wlb::snapshot::ModuleMessage message;
  wlb::snapshot::Snapshot* snapshot = message.mutable_snapshot();
  snapshot->set_bot("Dezlu");
  snapshot->set_seq(412);
  snapshot->set_time_ms(1794729600000);

  wlb::snapshot::Self* self = snapshot->mutable_base()->mutable_self();
  self->mutable_unit()->set_name("Dezlu");
  self->mutable_unit()->set_level(5);
  self->mutable_unit()->mutable_player()->set_class_("Priest");
  self->set_health(96);
  self->set_health_max(118);
  self->set_state(wlb::snapshot::Self::ALIVE);

  wlb::snapshot::Unit* boar =
      snapshot->mutable_base()->mutable_nearby()->add_units();
  boar->set_name("Mottled Boar");
  boar->set_reaction(wlb::snapshot::Unit::HOSTILE);
  boar->mutable_rel()->set_dist_yd(28.5f);
  boar->mutable_rel()->set_dir(wlb::snapshot::S);

  snapshot->add_events()->mutable_chat()->set_text("hi\nthere");
  wlb::snapshot::Option* option = snapshot->add_options();
  option->set_text("Buy a guild charter named ___");
  option->add_blanks()->set_kind(wlb::snapshot::Blank::NEW_NAME);
  return message;
}

TEST(SnapshotProtoTest, RoundTripsThroughFrames) {
  const wlb::snapshot::ModuleMessage sent = MakeSnapshotMessage();
  std::string stream;
  ASSERT_TRUE(net::AppendFrame(sent.SerializeAsString(), &stream));

  net::FrameDecoder decoder;
  const std::vector<std::string> frames = decoder.Append(stream);
  ASSERT_EQ(frames.size(), 1u);
  wlb::snapshot::ModuleMessage received;
  ASSERT_TRUE(received.ParseFromString(frames[0]));

  ASSERT_TRUE(received.has_snapshot());
  const wlb::snapshot::Snapshot& snapshot = received.snapshot();
  EXPECT_EQ(snapshot.bot(), "Dezlu");
  EXPECT_EQ(snapshot.time_ms(), 1794729600000);
  EXPECT_EQ(snapshot.base().self().unit().player().class_(), "Priest");
  EXPECT_EQ(snapshot.base().nearby().units(0).rel().dir(), wlb::snapshot::S);
  EXPECT_EQ(snapshot.events(0).chat().text(), "hi\nthere");
  EXPECT_EQ(snapshot.options(0).blanks(0).kind(),
            wlb::snapshot::Blank::NEW_NAME);
  EXPECT_EQ(received.SerializeAsString(), sent.SerializeAsString());
}

TEST(SnapshotProtoTest, DefaultsAreNotSerialized) {
  // Implicit presence (proto3 semantics): false / 0 / "" cost no bytes, which
  // keeps the 1 s snapshots small.
  wlb::snapshot::Self self;
  self.set_in_combat(false);
  self.set_money(0);
  EXPECT_TRUE(self.SerializeAsString().empty());
}

TEST(SnapshotProtoTest, CommandOneofCarriesBlankFills) {
  wlb::snapshot::Command command;
  command.set_bot("Zul'jinx");
  command.mutable_choose()->set_option(4);
  command.mutable_choose()->add_fills("Totem Pole");

  wlb::snapshot::Command parsed;
  ASSERT_TRUE(parsed.ParseFromString(command.SerializeAsString()));
  ASSERT_EQ(parsed.command_case(), wlb::snapshot::Command::kChoose);
  EXPECT_EQ(parsed.choose().option(), 4u);
  EXPECT_EQ(parsed.choose().fills(0), "Totem Pole");
}

}  // namespace
}  // namespace llm_brain
