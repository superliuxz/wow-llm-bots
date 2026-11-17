// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_loop.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "decision_log.h"
#include "file_util.h"
#include "gtest/gtest.h"
#include "llm_client.h"
#include "proto/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

// A canned LLM with two-phase streaming control: tests can fire `on_first_line`
// first, verify early `Command.Choose` dispatch, and finish `reply` afterwards.
class CannedBrain {
 public:
  struct PendingCall {
    LlmRequest request;
    base::OnceCallback<void(LlmResult)> reply;
    FirstLineCallback on_first_line;
    base::scoped_refptr<LlmCancelHandle> cancel;
  };

  AsyncAskFn Fn() {
    return [this](LlmRequest request, base::OnceCallback<void(LlmResult)> reply,
                  FirstLineCallback on_first_line) {
      auto cancel = base::MakeRefCounted<LlmCancelHandle>();
      calls_.push_back({std::move(request), std::move(reply),
                        std::move(on_first_line), cancel});
      return cancel;
    };
  }

  size_t size() const { return calls_.size(); }
  const LlmRequest& front_request() const { return calls_.front().request; }
  bool front_cancelled() const { return calls_.front().cancel->cancelled(); }

  void StreamFirstLine(const std::string& line) {
    ASSERT_FALSE(calls_.empty());
    if (calls_.front().on_first_line) {
      std::move(calls_.front().on_first_line).Run(line);
    }
  }

  // Like LlmClient: a cancelled call still replies, with `cancelled` set.
  void FinishCancelled() {
    ASSERT_FALSE(calls_.empty());
    PendingCall call = std::move(calls_.front());
    calls_.pop_front();
    LlmResult res;
    res.cancelled = true;
    res.error = "cancelled";
    std::move(call.reply).Run(std::move(res));
  }

  void FinishReply(const std::string& full_text,
                   double first_ms = 60.0,
                   double total_ms = 150.0) {
    ASSERT_FALSE(calls_.empty());
    if (calls_.front().cancel->cancelled()) {
      FinishCancelled();
      return;
    }
    PendingCall call = std::move(calls_.front());
    calls_.pop_front();
    LlmResult res;
    LlmReply rep;
    rep.text = full_text;
    rep.prompt_tokens = 800;
    rep.prompt_computed = 300;
    rep.completion_tokens = 15;
    res.reply = rep;
    res.first_line_ms = first_ms;
    res.round_trip_ms = total_ms;
    std::move(call.reply).Run(std::move(res));
  }

 private:
  std::deque<PendingCall> calls_;
};

pb::Snapshot MakeLiveSnap(const std::string& bot,
                          uint32_t seq,
                          base::TimeTicks built_at,
                          bool in_combat = false,
                          uint32_t hp = 60) {
  pb::Snapshot snap;
  snap.bot = bot;
  snap.seq = seq;
  snap.monotonic_us = (built_at - base::TimeTicks()).InMicroseconds();
  pb::Self& self = snap.base.self;
  self.state = pb::Self::ALIVE;
  self.in_combat = in_combat;
  self.health = hp;
  self.health_max = 60;
  self.power_type = pb::Self::MANA;
  self.power = 100;
  self.power_max = 100;
  return snap;
}

void AddUnitAndOption(pb::Snapshot* snap,
                      const std::string& guid,
                      const std::string& name,
                      bool dead,
                      bool lootable,
                      bool targeting_me = false) {
  pb::Unit& u = snap->base.nearby.units.emplace_back();
  u.guid = guid;
  u.name = name;
  u.kind = pb::Unit::NPC;
  u.reaction = pb::Unit::NEUTRAL;
  u.dead = dead;
  u.lootable = lootable;
  u.targeting_me = targeting_me;
  u.rel.dist_yd = 5.0f;
  if (dead && lootable) {
    pb::Option& opt = snap->options.emplace_back();
    opt.text = "Loot " + name;
    opt.guid = guid;
  } else if (!dead) {
    pb::Option& opt = snap->options.emplace_back();
    opt.text = "Attack " + name;
    opt.guid = guid;
  }
}

void AddKeep(pb::Snapshot* snap) {
  snap->options.emplace_back().text = "Keep doing what I'm doing";
}

TEST(BotLoopTest, EarlyDispatchesDoLineCarriesGoalAndExecutesAttackThenLoot) {
  std::string dir = testing::TempDir() + "wlb-botloop-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  ASSERT_TRUE(MakeDirectories(dir + "/Testpriest"));
  ASSERT_TRUE(
      AppendToFile(dir + "/Testpriest/identity.md", "Calm orc priest."));

  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.bots_dir = dir;
  cfg.max_in_flight = 4;

  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0);
  AddUnitAndOption(&s1, "0x1", "Mottled Boar", /*dead=*/false,
                   /*lootable=*/false);
  AddKeep(&s1);

  // First snapshot wakes the bot ("first look") and loads identity.md.
  loop.OnSnapshot(s1, t0 + base::Milliseconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_EQ(brain.front_request().system, "RULES");
  EXPECT_NE(brain.front_request().user.find("Calm orc priest."),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find("Woken: first look."),
            std::string::npos);

  // Stream line 1 ("do: 1") BEFORE the rest of the reply arrives -> Choose goes
  // out immediately!
  brain.StreamFirstLine("do: 1");
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].bot, "Testpriest");
  ASSERT_TRUE(commands[0].choose.has_value());
  EXPECT_EQ(commands[0].choose->seq, 1u);
  EXPECT_EQ(commands[0].choose->option, 1u);

  // Bot enters combat with Mottled Boar BEFORE full reply arrives ->
  // provisional expectation prevents a spurious "entered combat" wake!
  pb::Snapshot s2 = MakeLiveSnap("Testpriest", 2, t0 + base::Milliseconds(100),
                                 /*in_combat=*/true);
  AddUnitAndOption(&s2, "0x1", "Mottled Boar", /*dead=*/false,
                   /*lootable=*/false, /*targeting_me=*/true);
  AddKeep(&s2);
  loop.OnSnapshot(s2, t0 + base::Milliseconds(101));
  EXPECT_EQ(brain.size(), 1u);  // Still just the 1 in-flight call.

  // Full reply finishes with goal + watch: hp<35 + say -> does NOT re-send
  // Choose #1, sends Say, and saves goal.
  brain.FinishReply(
      "do: 1\nexpect: 1\nwatch: hp<35\nlook: 60\n"
      "goal: hunt boars\nsay: pulling boar");
  ASSERT_EQ(commands.size(), 2u);
  ASSERT_TRUE(commands[1].say.has_value());
  EXPECT_EQ(commands[1].say->text, "pulling boar");
  EXPECT_EQ(loop.CurrentGoal("Testpriest"), "hunt boars");
  EXPECT_EQ(loop.decisions_completed(), 1u);

  // Boar dies (lootable) + loot item event -> step 1 finishes ("plan done"),
  // waking the brain with `Current goal: hunt boars.` and `Since your last
  // look:`.
  pb::ImmediateEvent item_ev;
  item_ev.bot = "Testpriest";
  item_ev.event.item_received.emplace().item.emplace().name = "Tough Jerky";
  loop.OnEvent(item_ev, t0 + base::Seconds(5));
  EXPECT_EQ(brain.size(), 0u);  // Loot line does not wake by itself.

  pb::Snapshot s3 = MakeLiveSnap("Testpriest", 3, t0 + base::Seconds(6),
                                 /*in_combat=*/false);
  AddUnitAndOption(&s3, "0x1", "Mottled Boar", /*dead=*/true,
                   /*lootable=*/true);
  AddKeep(&s3);
  loop.OnSnapshot(s3, t0 + base::Seconds(6) + base::Milliseconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Current goal: hunt boars."),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find("Woken: plan done."),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find("received 1x Tough Jerky"),
            std::string::npos);

  // Brain loots the corpse (`do: 1`, omitting optional `goal:` -> goal stays!).
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: hp<35\nlook: 60");
  ASSERT_EQ(commands.size(), 3u);
  ASSERT_TRUE(commands[2].choose.has_value());
  EXPECT_EQ(commands[2].choose->seq, 3u);
  EXPECT_EQ(commands[2].choose->option, 1u);
  EXPECT_EQ(loop.CurrentGoal("Testpriest"), "hunt boars");

  // Clean up temp decision logs.
  DecisionLog dlog(dir);
  const std::string log_path = dlog.PathFor("Testpriest", base::Time::Now());
  const std::optional<std::string> log_text = ReadFile(log_path);
  ASSERT_TRUE(log_text);
  EXPECT_NE(log_text->find("\"age_at_ask_ms\""), std::string::npos);
  EXPECT_NE(log_text->find("\"age_at_plan_ms\""), std::string::npos);
  ::unlink(log_path.c_str());
  ::unlink((dir + "/Testpriest/identity.md").c_str());
  ::rmdir((dir + "/Testpriest/events").c_str());
  ::rmdir((dir + "/Testpriest").c_str());
  ::rmdir(dir.c_str());
}

TEST(BotLoopTest, WatchCrossingAndLookTimerWakeTheBot) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: hp<40\nlook: 30");
  EXPECT_EQ(brain.size(), 0u);

  // Look timer expires at t0 + 31s -> wakes with "look timer".
  loop.OnTick(base::TimeTicks::Now() + base::Seconds(31));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: look timer."),
            std::string::npos);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: hp<40\nlook: 60");

  // Entering combat AND dropping HP to 20/60 (33% < 40%) combines both reasons.
  pb::Snapshot low_hp = MakeLiveSnap("Tess", 2, t0 + base::Seconds(35),
                                     /*in_combat=*/true, /*hp=*/20);
  AddKeep(&low_hp);
  loop.OnSnapshot(low_hp, t0 + base::Seconds(35));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: entered combat; hp<40."),
            std::string::npos)
      << brain.front_request().user;
}

TEST(BotLoopTest, SupersededInFlightCallDoesNotDispatchStaleSteps) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddUnitAndOption(&s1, "0x1", "Mottled Boar", false, false);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);

  // While call #1 is thinking, Tess dies -> an urgent (P0) "died" wake cancels
  // the obsolete routine call and queues behind it.
  pb::Snapshot dead = MakeLiveSnap("Tess", 2, t0 + base::Milliseconds(50));
  dead.base.self.state = pb::Self::DEAD_BODY;
  AddKeep(&dead);
  loop.OnSnapshot(dead, t0 + base::Milliseconds(50));
  EXPECT_TRUE(brain.front_cancelled());

  // Call #1 still streams "do: 1" (Attack Boar) before the abort lands ->
  // suppressed because Tess already has a newer wake queued; its cancelled
  // reply frees the slot and call #2 starts immediately!
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: 1\nwatch: none\nlook: 60");
  EXPECT_TRUE(commands.empty());
  EXPECT_EQ(loop.calls_cancelled(), 1u);
  EXPECT_EQ(loop.decisions_completed(), 0u);
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: died."), std::string::npos);
  EXPECT_FALSE(brain.front_cancelled());
}

TEST(BotLoopTest, ARoutineWakeDoesNotCancelTheCallInFlight) {
  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);

  // A whisper (P3) while the first look is in flight waits; it doesn't abort.
  pb::ImmediateEvent whisper;
  whisper.bot = "Tess";
  pb::ChatEvent& chat = whisper.event.chat.emplace();
  chat.channel = pb::WHISPER;
  chat.from = "Will";
  chat.text = "hi";
  loop.OnEvent(whisper, t0 + base::Milliseconds(10));
  EXPECT_FALSE(brain.front_cancelled());
  EXPECT_EQ(loop.queued(), 1u);
}

TEST(BotLoopTest, ACancelledCallIsNotABrainErrorAndSetsNoRetryTimer) {
  CannedBrain brain;
  std::vector<std::string> logs;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(
      cfg, brain.Fn(), [](const pb::Command&) {},
      [&](const std::string& line) { logs.push_back(line); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishCancelled();
  EXPECT_EQ(loop.in_flight(), 0u);
  EXPECT_EQ(loop.calls_cancelled(), 1u);
  for (const std::string& line : logs) {
    EXPECT_EQ(line.find("brain error"), std::string::npos) << line;
  }
  // No 10 s "look timer" retry: nothing wakes until the world says so.
  loop.OnTick(base::TimeTicks::Now() + base::Seconds(11));
  EXPECT_EQ(brain.size(), 0u);
}

TEST(BotLoopTest, ReconnectAfterDisconnectWithInFlightCallWakesBotAgain) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);

  // Module disconnects while call #1 is still in flight: the call is aborted
  // (its answer would be for the old connection's screen) and replies
  // cancelled before any new snapshot arrives.
  loop.OnConnectionChanged(false);
  EXPECT_TRUE(brain.front_cancelled());
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  EXPECT_EQ(loop.in_flight(), 0u);

  // Reconnecting and receiving a new snapshot must trigger a fresh first look.
  loop.OnConnectionChanged(true);
  pb::Snapshot s2 = MakeLiveSnap("Tess", 2, t0 + base::Seconds(5));
  AddKeep(&s2);
  loop.OnSnapshot(s2, t0 + base::Seconds(5));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: first look."),
            std::string::npos);
}

TEST(BotLoopTest, StaleCommandErrorDoesNotCancelCurrentPlan) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  ASSERT_EQ(commands.size(), 1u);
  const uint32_t current_id = commands[0].command_id;

  // An error from an older command (different command_id) is ignored.
  pb::ImmediateEvent stale_err;
  stale_err.bot = "Tess";
  pb::ErrorEvent& se = stale_err.event.error.emplace();
  se.command_id = current_id + 99;
  se.text = "too late";
  loop.OnEvent(stale_err, t0 + base::Seconds(1));
  EXPECT_EQ(brain.size(), 0u);

  // An error matching the current plan's command_id cancels it and wakes.
  pb::ImmediateEvent cur_err;
  cur_err.bot = "Tess";
  pb::ErrorEvent& ce = cur_err.event.error.emplace();
  ce.command_id = current_id;
  ce.text = "target lost";
  loop.OnEvent(cur_err, t0 + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: step failed: target lost."),
            std::string::npos);
}

TEST(BotLoopTest, AWhisperWakesTheBotAndItsTellWhispersBack) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 300");
  ASSERT_EQ(brain.size(), 0u);

  pb::ImmediateEvent whisper;
  whisper.bot = "Testpriest";
  pb::ChatEvent& chat = whisper.event.chat.emplace();
  chat.channel = pb::WHISPER;
  chat.from = "Gatda";
  chat.text = "want to group?";
  chat.at_me = true;
  loop.OnEvent(whisper, t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("whisper from Gatda"),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find("want to group?"),
            std::string::npos);

  commands.clear();
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 300\n"
      "tell gatda: sure, send an invite");
  std::vector<pb::Command::Say> said;
  for (const pb::Command& cmd : commands) {
    if (cmd.say) {
      said.push_back(*cmd.say);
    }
  }
  ASSERT_EQ(said.size(), 1u);
  EXPECT_EQ(said[0].channel, pb::WHISPER);
  EXPECT_EQ(said[0].to, "Gatda");
  EXPECT_EQ(said[0].text, "sure, send an invite");
}

}  // namespace
}  // namespace harness
