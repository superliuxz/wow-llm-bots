// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/bot_loop.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "llm/llm_client.h"
#include "runtime/decision_log.h"
#include "runtime/file_util.h"
#include "snapshot/snapshot.h"
#include "test/gbnf_check.h"

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

  void FinishError(const std::string& error) {
    ASSERT_FALSE(calls_.empty());
    PendingCall call = std::move(calls_.front());
    calls_.pop_front();
    LlmResult res;
    res.error = error;
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
    opt.kind = pb::Option::LOOT;
    opt.name = name;
  } else if (!dead) {
    pb::Option& opt = snap->options.emplace_back();
    opt.text = "Attack " + name;
    opt.guid = guid;
    opt.kind = pb::Option::ATTACK;
    opt.name = name;
  }
}

void AddKeep(pb::Snapshot* snap) {
  pb::Option& opt = snap->options.emplace_back();
  opt.text = "Keep doing what I'm doing";
  opt.kind = pb::Option::KEEP;
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
  // Ticks run past the last snapshot, which live snapshots never let happen.
  cfg.stale_snapshot_after = base::Seconds(3600);
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
  EXPECT_NE(brain.front_request().user.find("Woken: first look; died."),
            std::string::npos);
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
  // The grammar lets it answer Gatda, and only Gatda.
  const std::string& grammar = brain.front_request().grammar;
  EXPECT_TRUE(
      GbnfAccepts(
          grammar, "root",
          "do: 1\nexpect: none\nwatch: none\nlook: 60\ntell Gatda: sure")
          .accepted);
  EXPECT_FALSE(
      GbnfAccepts(
          grammar, "root",
          "do: 1\nexpect: none\nwatch: none\nlook: 60\ntell Kelra: sure")
          .accepted);

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

pb::ImmediateEvent WhisperFrom(const std::string& bot,
                               const std::string& from,
                               const std::string& text) {
  pb::ImmediateEvent whisper;
  whisper.bot = bot;
  pb::ChatEvent& chat = whisper.event.chat.emplace();
  chat.channel = pb::WHISPER;
  chat.from = from;
  chat.text = text;
  chat.at_me = true;
  return whisper;
}

// A whisper's call is cancelled by a more urgent wake: the next call still
// carries the whisper (reason and text), so it can be answered.
TEST(BotLoopTest, AWhisperSurvivesItsCallBeingCancelled) {
  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 300");

  loop.OnEvent(WhisperFrom("Tess", "Gatda", "want to group?"),
               t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);

  pb::Snapshot dead = MakeLiveSnap("Tess", 2, t0 + base::Seconds(2));
  dead.base.self.state = pb::Self::DEAD_BODY;
  AddKeep(&dead);
  loop.OnSnapshot(dead, t0 + base::Seconds(2));
  ASSERT_TRUE(brain.front_cancelled());
  brain.FinishCancelled();

  ASSERT_EQ(brain.size(), 1u);
  const std::string& user = brain.front_request().user;
  EXPECT_NE(user.find("died"), std::string::npos) << user;
  EXPECT_NE(user.find("whisper from Gatda"), std::string::npos) << user;
  EXPECT_NE(user.find("want to group?"), std::string::npos) << user;
}

// An invalid plan in answer to a whisper: the retry still carries it.
TEST(BotLoopTest, AWhisperSurvivesAnInvalidPlan) {
  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  // Ticks run past the last snapshot, which live snapshots never let happen.
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 300");

  loop.OnEvent(WhisperFrom("Tess", "Gatda", "want to group?"),
               t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("this is not a plan");
  ASSERT_EQ(brain.size(), 0u);

  loop.OnTick(t0 + base::Seconds(30));
  ASSERT_EQ(brain.size(), 1u);
  const std::string& user = brain.front_request().user;
  EXPECT_NE(user.find("whisper from Gatda"), std::string::npos) << user;
  EXPECT_NE(user.find("want to group?"), std::string::npos) << user;
}

TEST(BotLoopTest, StepRefusalBackoffDoublesFromTheSecondAndCapsAt30s) {
  EXPECT_EQ(StepRefusalBackoff(1), base::TimeDelta());
  EXPECT_EQ(StepRefusalBackoff(2), base::Seconds(2));
  EXPECT_EQ(StepRefusalBackoff(3), base::Seconds(4));
  EXPECT_EQ(StepRefusalBackoff(5), base::Seconds(16));
  EXPECT_EQ(StepRefusalBackoff(6), base::Seconds(30));
  EXPECT_EQ(StepRefusalBackoff(50), base::Seconds(30));
}

// A body that refuses every step (dead, out of range) wakes the brain at once
// the first time, then only after a growing delay.
TEST(BotLoopTest, RepeatedStepRefusalsBackOff) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  // Ticks run past the last snapshot, which live snapshots never let happen.
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddUnitAndOption(&s1, "0x1", "Mottled Boar", false, false);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);

  auto refuse = [&](base::TimeTicks at) {
    ASSERT_FALSE(commands.empty());
    pb::ImmediateEvent err;
    err.bot = "Tess";
    pb::ErrorEvent& e = err.event.error.emplace();
    e.command_id = commands.back().command_id;
    e.text = "you are dead";
    loop.OnEvent(err, at);
  };

  // Refusal 1: an immediate wake.
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse(t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);

  // Refusal 2: no call until 2 s later, and the reason is kept.
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse(t0 + base::Seconds(2));
  EXPECT_EQ(brain.size(), 0u);
  loop.OnTick(t0 + base::Seconds(3));
  EXPECT_EQ(brain.size(), 0u);
  loop.OnTick(t0 + base::Seconds(4));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("step failed: you are dead"),
            std::string::npos);

  // Refusal 3: 4 s.
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse(t0 + base::Seconds(5));
  loop.OnTick(t0 + base::Seconds(8));
  EXPECT_EQ(brain.size(), 0u);
  loop.OnTick(t0 + base::Seconds(9));
  EXPECT_EQ(brain.size(), 1u);
}

// A bot whose snapshots stopped (logged out) isn't asked about its last
// screen; when they come back it gets a first look.
TEST(BotLoopTest, ABotWhoseSnapshotsStoppedIsNotAskedAboutItsLastScreen) {
  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  ASSERT_EQ(brain.size(), 0u);

  loop.OnTick(t0 + base::Seconds(6));    // Stale: forgotten.
  loop.OnTick(t0 + base::Seconds(120));  // Past the look timer: no call.
  EXPECT_EQ(brain.size(), 0u);

  pb::Snapshot back = MakeLiveSnap("Tess", 2, t0 + base::Seconds(130));
  AddKeep(&back);
  loop.OnSnapshot(back, t0 + base::Seconds(130));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("first look"), std::string::npos);
}

// An early-dispatched step refused while the same LLM call is still streaming
// its tail must still deliver say/tell from that reply, must not requeue the
// already-answered wake reasons on refusal #1, and must keep the backoff
// deadline instead of overwriting it with `look:` on refusal #2.
TEST(BotLoopTest, EarlyStepRefusedDuringStreamingKeepsTellAndBackoff) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddUnitAndOption(&s1, "0x1", "Mottled Boar", false, false);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 2\nexpect: none\nwatch: none\nlook: 300");

  auto refuse_last = [&](base::TimeTicks at) {
    ASSERT_FALSE(commands.empty());
    pb::ImmediateEvent err;
    err.bot = "Tess";
    pb::ErrorEvent& e = err.event.error.emplace();
    e.command_id = commands.back().command_id;
    e.text = "you are dead";
    loop.OnEvent(err, at);
  };

  // Call woken by a whisper: `do: 1` streams early, and the module refuses it
  // before the rest of the reply arrives.
  loop.OnEvent(WhisperFrom("Tess", "Brakka", "coming?"), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  refuse_last(t0 + base::Seconds(1) + base::Milliseconds(10));

  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\ntell Brakka: omw");
  bool sent_tell = false;
  for (const pb::Command& cmd : commands) {
    if (cmd.say && cmd.say->channel == pb::WHISPER && cmd.say->to == "Brakka" &&
        cmd.say->text == "omw") {
      sent_tell = true;
    }
  }
  EXPECT_TRUE(sent_tell);

  // Refusal #1 wakes at once, without requeueing the answered whisper reason.
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Woken: step failed: you are dead."),
      std::string::npos)
      << brain.front_request().user;
  EXPECT_EQ(brain.front_request().user.find("Woken: whisper from Brakka"),
            std::string::npos)
      << brain.front_request().user;

  // Refusal #2 during streaming: FinishReply must not overwrite the 2 s
  // backoff deadline with `look: 60`, and the timer wake keeps the reason.
  brain.StreamFirstLine("do: 1");
  const base::TimeTicks r2_at = t0 + base::Seconds(2);
  refuse_last(r2_at);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  EXPECT_EQ(brain.size(), 0u);

  loop.OnTick(r2_at + base::Seconds(1));
  EXPECT_EQ(brain.size(), 0u);
  loop.OnTick(r2_at + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Woken: step failed: you are dead."),
      std::string::npos)
      << brain.front_request().user;
  EXPECT_EQ(brain.front_request().user.find("look timer"), std::string::npos)
      << brain.front_request().user;
}

TEST(BotLoopTest, LookTimerAfterKeepResetsStepRefusals) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddUnitAndOption(&s1, "0x1", "Mottled Boar", false, false);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);

  auto refuse_last = [&](base::TimeTicks at) {
    ASSERT_FALSE(commands.empty());
    pb::ImmediateEvent err;
    err.bot = "Tess";
    pb::ErrorEvent& e = err.event.error.emplace();
    e.command_id = commands.back().command_id;
    e.text = "target lost";
    loop.OnEvent(err, at);
  };

  // Refusal 1 wakes immediately; the brain then picks Keep for 10 s.
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse_last(t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 2");
  brain.FinishReply("do: 2\nexpect: none\nwatch: none\nlook: 10");
  EXPECT_EQ(brain.size(), 0u);

  // Completing the Keep look timer resets the consecutive-refusal counter.
  loop.OnTick(t0 + base::Seconds(20));
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse_last(t0 + base::Seconds(21));
  EXPECT_EQ(brain.size(), 1u);
}

// Any wake that isn't for a refusal starts the count over: a refusal after it
// wakes the brain at once (review of f40b0b6, finding 6: one late refusal
// held a bot in combat back for up to 30 s).
TEST(BotLoopTest, AnotherWakeResetsStepRefusals) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddUnitAndOption(&s1, "0x1", "Mottled Boar", false, false);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  auto refuse_last = [&](base::TimeTicks at) {
    ASSERT_FALSE(commands.empty());
    pb::ImmediateEvent err;
    err.bot = "Tess";
    pb::ErrorEvent& e = err.event.error.emplace();
    e.command_id = commands.back().command_id;
    e.text = "target lost";
    loop.OnEvent(err, at);
  };
  // Two refusals in a row: the second backs off.
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse_last(t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse_last(t0 + base::Seconds(2));
  EXPECT_EQ(brain.size(), 0u);  // Backing off.
  // A whisper wakes the bot meanwhile; its plan's step is refused: at once.
  loop.OnEvent(WhisperFrom("Tess", "Brakka", "hi"), t0 + base::Seconds(3));
  ASSERT_EQ(brain.size(), 1u);
  brain.StreamFirstLine("do: 1");
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  refuse_last(t0 + base::Seconds(4));
  EXPECT_EQ(brain.size(), 1u);
}

TEST(BotLoopTest, StaleSnapshotWhileCallInFlightDoesNotRequeueOldReasons) {
  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Tess", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  loop.OnEvent(WhisperFrom("Tess", "Brakka", "old question"),
               t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);

  // Snapshots stop while call #2 is still in flight -> forgotten, and the late
  // reply must not requeue the old whisper onto the next login.
  loop.OnTick(t0 + base::Seconds(6));
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  pb::Snapshot back = MakeLiveSnap("Tess", 2, t0 + base::Seconds(10));
  AddKeep(&back);
  loop.OnSnapshot(back, t0 + base::Seconds(10));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: first look."),
            std::string::npos)
      << brain.front_request().user;
  EXPECT_EQ(brain.front_request().user.find("whisper from Brakka"),
            std::string::npos)
      << brain.front_request().user;
  EXPECT_EQ(brain.front_request().user.find("old question"), std::string::npos)
      << brain.front_request().user;
}

std::vector<pb::Command> PanelCommands(const std::vector<pb::Command>& all) {
  std::vector<pb::Command> out;
  for (const pb::Command& c : all) {
    if (c.open || c.close || c.hover) {
      out.push_back(c);
    }
  }
  return out;
}

void OpenBags(pb::Snapshot* snap) {
  snap->base.ui.open_panels.push_back(pb::BAGS);
  pb::Bags& bags = snap->ext.bags.emplace();
  bags.free_slots = 10;
  bags.total_slots = 16;
  pb::Item& bread = bags.items.emplace_back();
  bread.id = 4540;
  bread.name = "Tough Hunk of Bread";
  bread.count = 4;
  bread.kind = pb::Item::CONSUMABLE;
}

TEST(BotLoopTest, OpenWakesTheBrainOnceShownAndClosedPanelsAreRemembered) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);  // A long tick below.
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0);
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60\nopen: bags");
  std::vector<pb::Command> panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 1u);
  ASSERT_TRUE(panel[0].open);
  EXPECT_EQ(panel[0].open->panel, pb::BAGS);

  // The next snapshot shows the bags: the brain is woken to read them.
  pb::Snapshot s2 = MakeLiveSnap("Testpriest", 2, t0 + base::Seconds(1));
  AddKeep(&s2);
  OpenBags(&s2);
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Woken: looked at bags."),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find("Tough Hunk of Bread x4"),
            std::string::npos);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60\nclose: bags");
  panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 2u);
  ASSERT_TRUE(panel[1].close);

  // Closed: the next look remembers them, with their age.
  pb::Snapshot s3 = MakeLiveSnap("Testpriest", 3, t0 + base::Seconds(2));
  AddKeep(&s3);
  loop.OnSnapshot(s3, t0 + base::Seconds(2));
  loop.OnTick(t0 + base::Seconds(70));
  ASSERT_EQ(brain.size(), 1u);
  const std::string& user = brain.front_request().user;
  EXPECT_NE(user.find("You remember (not on screen now):\nbags, seen "),
            std::string::npos)
      << user;
  EXPECT_NE(user.find("Tough Hunk of Bread x4"), std::string::npos);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // After remember_panels_for (5 min), they are forgotten.
  pb::Snapshot s4 = MakeLiveSnap("Testpriest", 4, t0 + base::Seconds(399));
  AddKeep(&s4);
  loop.OnSnapshot(s4, t0 + base::Seconds(399));
  loop.OnTick(t0 + base::Seconds(400));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_EQ(brain.front_request().user.find("You remember"), std::string::npos)
      << brain.front_request().user;
}

// `open:` of a panel already on the brain's screen shows nothing new: no wake
// for it (review of f40b0b6, finding 5: it woke on every snapshot).
TEST(BotLoopTest, OpenOfAPanelAlreadyOpenDoesNotWake) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0);
  AddKeep(&s1);
  OpenBags(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60\nopen: bags");
  for (int i = 2; i < 12; ++i) {
    pb::Snapshot s = MakeLiveSnap("Testpriest", i, t0 + base::Seconds(i));
    AddKeep(&s);
    OpenBags(&s);
    loop.OnSnapshot(s, t0 + base::Seconds(i));
  }
  EXPECT_EQ(brain.size(), 0u);
}

TEST(BotLoopTest, HoverSendsTheThingsRefAndAnUnknownNameNothing) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0);
  AddKeep(&s1);
  OpenBags(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().grammar.find("\"Tough Hunk of Bread\""),
            std::string::npos);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\nhover: Tough Hunk of Bread");
  std::vector<pb::Command> panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 1u);
  ASSERT_TRUE(panel[0].hover && panel[0].hover->ref);
  EXPECT_EQ(panel[0].hover->ref->kind, pb::HoverRef::ITEM);
  EXPECT_EQ(panel[0].hover->ref->id, "4540");

  // The tooltip shows: the brain is woken to read it.
  pb::Snapshot s2 = MakeLiveSnap("Testpriest", 2, t0 + base::Seconds(1));
  AddKeep(&s2);
  OpenBags(&s2);
  pb::Tooltipped& tip = s2.base.tooltips.emplace_back();
  tip.ref = panel[0].hover->ref;
  tip.tooltip.emplace().lines = {"Tough Hunk of Bread",
                                 "Use: Restores 61 health over 18 sec."};
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find(
                "pointing at Tough Hunk of Bread: Use: Restores 61 health"),
            std::string::npos);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\nhover: Peacebloom");
  EXPECT_EQ(PanelCommands(commands).size(), 1u);  // Not on screen: nothing.
}

TEST(BotLoopTest, HabitsCloseInCombatAndGlanceAtTheQuestLog) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const auto quest = [](pb::Snapshot* snap, bool complete) {
    pb::Objective& o = snap->ext.quest_tracker.emplace_back();
    o.title = "Kill boars";
    pb::QuestPart& q = o.quest.emplace();
    q.quest_id = 7;
    q.quest = "Boar Hunt";
    q.complete = complete;
  };
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0);
  AddKeep(&s1);
  quest(&s1, false);
  loop.OnSnapshot(s1, t0);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // The quest is complete: a glance at the quest log, closed once seen.
  pb::Snapshot s2 = MakeLiveSnap("Testpriest", 2, t0 + base::Seconds(1));
  AddKeep(&s2);
  quest(&s2, true);
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  std::vector<pb::Command> panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 1u);
  ASSERT_TRUE(panel[0].open);
  EXPECT_EQ(panel[0].open->panel, pb::QUEST_LOG);
  pb::Snapshot s3 = MakeLiveSnap("Testpriest", 3, t0 + base::Seconds(2));
  AddKeep(&s3);
  quest(&s3, true);
  s3.base.ui.open_panels.push_back(pb::QUEST_LOG);
  s3.ext.quest_log.emplace();
  loop.OnSnapshot(s3, t0 + base::Seconds(2));
  panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 2u);
  ASSERT_TRUE(panel[1].close);
  EXPECT_EQ(panel[1].close->panel, pb::QUEST_LOG);
  EXPECT_EQ(brain.size(), 0u);  // Habits ask the brain nothing.

  // Combat starts with bags open that no habit opened (the brain or the
  // player did): they stay open (review of f40b0b6, finding 8).
  pb::Snapshot s4 = MakeLiveSnap("Testpriest", 4, t0 + base::Seconds(3),
                                 /*in_combat=*/true);
  AddKeep(&s4);
  quest(&s4, true);
  OpenBags(&s4);
  loop.OnSnapshot(s4, t0 + base::Seconds(3));
  EXPECT_EQ(PanelCommands(commands).size(), 2u);
}

// The rest of finding 8 (review of f40b0b6): the first quest taken glances
// too, a glance never shown is given up (the habit works again), a glance
// cut by combat is closed, and each act is a line in the next prompt.
TEST(BotLoopTest, HabitsGlanceAtTheFirstQuestRetryAndTellTheBrain) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const auto snap = [&](int seq, int quest_id, bool in_combat = false) {
    pb::Snapshot s =
        MakeLiveSnap("Testpriest", seq, t0 + base::Seconds(seq), in_combat);
    AddKeep(&s);
    if (quest_id != 0) {
      pb::Objective& o = s.ext.quest_tracker.emplace_back();
      o.quest.emplace().quest_id = quest_id;
      o.quest->quest = "Boar Hunt";
    }
    return s;
  };
  loop.OnSnapshot(snap(1, 0), t0 + base::Seconds(1));
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  // The first quest: a glance.
  loop.OnSnapshot(snap(2, 7), t0 + base::Seconds(2));
  ASSERT_EQ(PanelCommands(commands).size(), 1u);
  // Never shown: given up after look_wait (3 s); the next change glances again.
  loop.OnSnapshot(snap(6, 7), t0 + base::Seconds(6));
  loop.OnSnapshot(snap(7, 8), t0 + base::Seconds(7));
  std::vector<pb::Command> panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 2u);
  ASSERT_TRUE(panel[1].open);
  // Combat starts while that glance is open: it is closed.
  pb::Snapshot fight = snap(8, 8, /*in_combat=*/true);
  fight.base.ui.open_panels.push_back(pb::QUEST_LOG);
  loop.OnSnapshot(fight, t0 + base::Seconds(8));
  panel = PanelCommands(commands);
  ASSERT_EQ(panel.size(), 3u);
  ASSERT_TRUE(panel[2].close);
  EXPECT_EQ(panel[2].close->panel, pb::QUEST_LOG);
  // The brain hears what its body did.
  loop.OnTick(t0 + base::Seconds(70));
  ASSERT_EQ(brain.size(), 1u);
  const std::string& user = brain.front_request().user;
  EXPECT_NE(user.find("you glanced at your quest log (your quests changed)"),
            std::string::npos)
      << user;
  EXPECT_NE(user.find("you closed your quest log (combat started)"),
            std::string::npos)
      << user;
}

TEST(BotLoopTest, NotesAndScreenNamesFeedBothPlanGrammarAndParsePlan) {
  std::string dir = testing::TempDir() + "harness-botloop-notes-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  ASSERT_EQ(::mkdir((dir + "/Testpriest").c_str(), 0700), 0);
  ASSERT_TRUE(
      AppendToFile(dir + "/Testpriest/notes.md",
                   "The Lich King, Defile: use Lay on Hands when it appears."));

  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.bots_dir = dir;
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0, /*in_combat=*/true);
  AddKeep(&s1);
  pb::Unit& boar = s1.base.nearby.units.emplace_back();
  boar.guid = "0xF130000C1A000001";
  boar.name = "Mottled Boar";
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  const LlmRequest& req = brain.front_request();
  EXPECT_NE(req.user.find("Your boss notes:"), std::string::npos);
  EXPECT_NE(req.grammar.find("\"Defile\""), std::string::npos);
  EXPECT_NE(req.grammar.find("\"Lay on Hands\""), std::string::npos);
  EXPECT_NE(req.grammar.find("\"Mottled Boar\""), std::string::npos);

  // `ParsePlan` receives the same `order_names`: "Lay on Hands" is read
  // as the spell name, not "Lay" on "Hands on Mottled Boar".
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: Defile on me -> use Lay on Hands on Mottled Boar");

  pb::Snapshot s2 = s1;
  s2.seq = 2;
  pb::Aura defile;
  defile.name = "Defile";
  s2.base.self.unit.auras.push_back(std::move(defile));
  loop.OnSnapshot(s2, t0 + base::Seconds(1));

  const pb::Command* act_cmd = nullptr;
  for (const pb::Command& cmd : commands) {
    if (cmd.act) {
      act_cmd = &cmd;
    }
  }
  ASSERT_NE(act_cmd, nullptr);
  EXPECT_EQ(act_cmd->act->verb, pb::Command::Act::USE);
  EXPECT_EQ(act_cmd->act->name, "Lay on Hands");
  EXPECT_EQ(act_cmd->act->target_name, "Mottled Boar");

  ::unlink((dir + "/Testpriest/notes.md").c_str());
  const std::string log_dir = dir + "/Testpriest/events";
  if (std::optional<std::vector<std::string>> entries; true) {
    // DecisionLog wrote one jsonl under events/; clean up best-effort.
    DecisionLog log(dir);
    (void)log;
  }
}

TEST(BotLoopTest, NewPlanDuringKiteSendsStopMovingAndShowsActiveOrders) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0, /*in_combat=*/true);
  AddKeep(&s1);
  pb::Unit& boar = s1.base.nearby.units.emplace_back();
  boar.guid = "0xF130000C1A000001";
  boar.name = "Mottled Boar";
  pb::Aura renew;
  renew.name = "Renew";
  s1.base.self.unit.auras.push_back(renew);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: Renew on me -> kite nearest Mottled Boar");

  // Trigger kite.
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
    return c.act && c.act->verb == pb::Command::Act::KITE;
  }));

  // Wake the bot via whisper while kiting; prompt must show active orders, and
  // installing the new plan must send `stop moving`.
  pb::ImmediateEvent imm;
  imm.bot = "Testpriest";
  pb::ChatEvent& chat = imm.event.chat.emplace();
  chat.channel = pb::WHISPER;
  chat.from = "Telda";
  chat.text = "stop";
  loop.OnEvent(std::move(imm), t0 + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Active standing orders:"),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find(
                "Renew on me -> kite nearest Mottled Boar"),
            std::string::npos);

  const size_t cmds_before = commands.size();
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60\norders: none");
  bool saw_stop_moving = false;
  for (size_t i = cmds_before; i < commands.size(); ++i) {
    if (commands[i].act && commands[i].act->verb == pb::Command::Act::STOP &&
        commands[i].act->stop == pb::Command::Act::MOVING) {
      saw_stop_moving = true;
    }
  }
  EXPECT_TRUE(saw_stop_moving);
}

TEST(BotLoopTest, OrderFailureBacksOffOnRepeat) {
  CannedBrain brain;
  std::vector<pb::Command> commands;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { commands.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Testpriest", 1, t0, /*in_combat=*/true);
  AddKeep(&s1);
  pb::Aura renew;
  renew.name = "Renew";
  s1.base.self.unit.auras.push_back(renew);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: Renew on me -> use NonexistentSpell on me");

  // First failure wakes immediately.
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_FALSE(commands.empty());
  const uint32_t act1_id = commands.back().command_id;
  pb::ImmediateEvent err1;
  err1.bot = "Testpriest";
  err1.event.error.emplace().command_id = act1_id;
  err1.event.error->text = "cannot cast";
  loop.OnEvent(std::move(err1), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);

  // Install a different order so it fires again and fails a second time.
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: Renew on me -> use OtherBadSpell on me");
  pb::Snapshot s3 = s1;
  s3.seq = 3;
  loop.OnSnapshot(s3, t0 + base::Seconds(2));
  const uint32_t act2_id = commands.back().command_id;
  pb::ImmediateEvent err2;
  err2.bot = "Testpriest";
  err2.event.error.emplace().command_id = act2_id;
  err2.event.error->text = "cannot cast";
  loop.OnEvent(std::move(err2), t0 + base::Seconds(2));
  // Second failure in a row backs off instead of waking immediately.
  EXPECT_EQ(brain.size(), 0u);
}

TEST(BotLoopTest, FlickeringOrderConditionDoesNotResetFailureBackoff) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s = MakeLiveSnap("Aelindra", 1, t0, /*in_combat=*/true);
  AddKeep(&s);
  s.base.self.unit.health_pct = 80.0f;
  loop.OnSnapshot(s, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: me hp<50 -> use Healthstone");

  // Failure #1 at t+1s wakes immediately.
  s.seq = 2;
  s.base.self.unit.health_pct = 40.0f;
  loop.OnSnapshot(s, t0 + base::Seconds(1));
  ASSERT_FALSE(sent.empty());
  const uint32_t act1_id = sent.back().command_id;
  pb::ImmediateEvent err1;
  err1.bot = "Aelindra";
  err1.event.error.emplace().command_id = act1_id;
  err1.event.error->text = "cannot cast";
  loop.OnEvent(std::move(err1), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: me hp<50 -> use Healthstone");

  // Brief quiet snapshot (1 s) must NOT reset order_wakes.
  s.seq = 3;
  s.base.self.unit.health_pct = 55.0f;
  loop.OnSnapshot(s, t0 + base::Seconds(2));

  // Condition flickers back at t+3s and fails again: must back off!
  s.seq = 4;
  s.base.self.unit.health_pct = 40.0f;
  loop.OnSnapshot(s, t0 + base::Seconds(3));
  const uint32_t act2_id = sent.back().command_id;
  pb::ImmediateEvent err2;
  err2.bot = "Aelindra";
  err2.event.error.emplace().command_id = act2_id;
  err2.event.error->text = "cannot cast";
  loop.OnEvent(std::move(err2), t0 + base::Seconds(3));
  EXPECT_EQ(brain.size(), 0u);
}

TEST(BotLoopTest, OrderCancelEventMidStreamDoesNotDropInFlightPlanOrTell) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s = MakeLiveSnap("Aelindra", 1, t0, /*in_combat=*/true);
  AddKeep(&s);
  s.base.self.unit.health_pct = 80.0f;
  loop.OnSnapshot(s, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: me hp<50 -> away 15");

  // Start the movement order.
  s.seq = 2;
  s.base.self.unit.health_pct = 40.0f;
  loop.OnSnapshot(s, t0 + base::Seconds(1));
  ASSERT_FALSE(sent.empty());
  const uint32_t move_act_id = sent.back().command_id;

  // Whisper wakes the brain so a call is in flight.
  pb::ImmediateEvent wh;
  wh.bot = "Aelindra";
  pb::ChatEvent& chat = wh.event.chat.emplace();
  chat.channel = pb::WHISPER;
  chat.from = "Thrall";
  chat.text = "hello";
  loop.OnEvent(std::move(wh), t0 + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);

  // Cancel event for the old movement act arrives while the call is in flight.
  pb::ImmediateEvent cancel;
  cancel.bot = "Aelindra";
  cancel.event.error.emplace().command_id = move_act_id;
  cancel.event.error->text = "cancelled by new choice";
  loop.OnEvent(std::move(cancel), t0 + base::Seconds(2));

  // The in-flight call must NOT be cancelled; answering it delivers its `tell`.
  ASSERT_EQ(brain.size(), 1u);
  sent.clear();
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 30\ntell Thrall: hey!");
  bool sent_tell = false;
  for (const pb::Command& cmd : sent) {
    if (cmd.say && cmd.say->channel == pb::WHISPER && cmd.say->to == "Thrall" &&
        cmd.say->text == "hey!") {
      sent_tell = true;
    }
  }
  EXPECT_TRUE(sent_tell);
}

TEST(BotLoopTest, OrderFailureDuringBrainCallWakesAfterReply) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s = MakeLiveSnap("Aelindra", 1, t0, /*in_combat=*/true);
  AddKeep(&s);
  s.base.self.unit.health_pct = 80.0f;
  loop.OnSnapshot(s, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: me hp<50 -> use Healthstone");

  // Start the potion order.
  s.seq = 2;
  s.base.self.unit.health_pct = 40.0f;
  loop.OnSnapshot(s, t0 + base::Seconds(1));
  const uint32_t act_id = sent.back().command_id;

  // Whisper starts a brain call.
  pb::ImmediateEvent wh;
  wh.bot = "Aelindra";
  pb::ChatEvent& chat = wh.event.chat.emplace();
  chat.channel = pb::WHISPER;
  chat.from = "Thrall";
  chat.text = "hi";
  loop.OnEvent(std::move(wh), t0 + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);

  // Real order failure arrives while the brain call is in flight.
  pb::ImmediateEvent fail;
  fail.bot = "Aelindra";
  fail.event.error.emplace().command_id = act_id;
  fail.event.error->text = "Healthstone is on cooldown";
  loop.OnEvent(std::move(fail), t0 + base::Seconds(2));

  // Finishing the call installs its plan AND wakes immediately for the failure
  // instead of waiting 60 s for the look timer.
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\ntell Thrall: hey!");
  EXPECT_EQ(brain.size(), 1u);
}

TEST(BotLoopTest,
     PrePullCompilesOrdersOnInstanceEntryCachesAndPreservesInCombat) {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "wlb_bot_loop_a4_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir / "Loada");
  {
    std::ofstream out(tmp_dir / "Loada" / "notes.md");
    out << "# Magisters' Terrace\n"
        << "- **Flame Strike**: fiery ground AoE.\n"
        << "  - Standing order: `in Flame Strike -> out of it`\n";
  }

  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.bots_dir = tmp_dir.string();
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  // Enter instance out of combat: triggers urgent pre-pull compilation.
  pb::Snapshot s1 = MakeLiveSnap("Loada", 1, t0, /*in_combat=*/false);
  s1.base.where.in_instance = true;
  s1.base.where.map = "Magisters' Terrace";
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("compile standing orders"),
            std::string::npos);

  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: hp<30\nlook: 60\n"
      "order: in Flame Strike -> out of it");

  // Mid-fight wake returning empty orders must preserve compiled boss orders.
  pb::Snapshot s2 = MakeLiveSnap("Loada", 2, t0 + base::Seconds(1),
                                 /*in_combat=*/true);
  s2.base.where.in_instance = true;
  s2.base.where.map = "Magisters' Terrace";
  s2.base.self.unit.health_pct = 20.0f;
  AddKeep(&s2);
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // Standing order still fires when Flame Strike appears under the bot.
  pb::Snapshot s3 = s2;
  s3.seq = 3;
  pb::Area fs;
  fs.guid = "trigger:0xF13000605A000001";
  fs.name = "Flame Strike";
  fs.radius_yd = 8.0f;
  fs.inside_me = true;
  fs.hostile = true;
  fs.rel.emplace().dist_yd = 1.0f;
  s3.base.nearby.areas.push_back(fs);
  const size_t cmds_before = sent.size();
  loop.OnSnapshot(s3, t0 + base::Seconds(2));
  bool saw_out_of = false;
  for (size_t i = cmds_before; i < sent.size(); ++i) {
    if (sent[i].act && sent[i].act->verb == pb::Command::Act::OUT_OF) {
      saw_out_of = true;
    }
  }
  EXPECT_TRUE(saw_out_of);

  // Reconnect into the same instance: cache hit installs orders immediately
  // without needing a new compilation before they work.
  loop.OnConnectionChanged(false);
  loop.OnConnectionChanged(true);
  pb::Snapshot s4 = s3;
  s4.seq = 4;
  s4.base.self.in_combat = true;
  const size_t cmds_before_cache = sent.size();
  loop.OnSnapshot(s4, t0 + base::Seconds(3));
  // First snapshot after reconnect installs cached orders and also wakes
  // first look; next snapshot fires cached order immediately.
  pb::Snapshot s5 = s4;
  s5.seq = 5;
  loop.OnSnapshot(s5, t0 + base::Seconds(4));
  bool saw_cached_out_of = false;
  for (size_t i = cmds_before_cache; i < sent.size(); ++i) {
    if (sent[i].act && sent[i].act->verb == pb::Command::Act::OUT_OF) {
      saw_cached_out_of = true;
    }
  }
  EXPECT_TRUE(saw_cached_out_of);

  std::filesystem::remove_all(tmp_dir);
}

TEST(
    BotLoopTest,
    OrdersStayAcrossEmptyPlansInAndOutOfCombatClearWithOrdersNoneAndCacheNotOverwrittenMidFight) {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "wlb_bot_loop_m6_m7_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir / "Loada");
  {
    std::ofstream out(tmp_dir / "Loada" / "notes.md");
    out << "# Magisters' Terrace\n"
        << "- **Flame Strike**: fiery ground AoE.\n"
        << "- **Phoenix**: fiery add.\n";
  }

  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  cfg.bots_dir = tmp_dir.string();
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });

  const base::TimeTicks t0 = base::TimeTicks::Now();
  // Enter instance out of combat: compile 2 pre-pull standing orders.
  pb::Snapshot s1 = MakeLiveSnap("Loada", 1, t0, /*in_combat=*/false);
  s1.base.where.in_instance = true;
  s1.base.where.map = "Magisters' Terrace";
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: in Flame Strike -> out of it\n"
      "order: Phoenix appears -> target nearest Phoenix");

  // Out-of-combat plan with no `order:` lines must KEEP the 2 pre-pull orders.
  pb::ImmediateEvent wh;
  wh.bot = "Loada";
  wh.event.chat.emplace().channel = pb::WHISPER;
  wh.event.chat->from = "Friend";
  wh.event.chat->text = "ready?";
  loop.OnEvent(std::move(wh), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // Mid-fight wake replaces orders with a 1-order plan; this must NOT
  // overwrite the 2-order pre-pull cache.
  pb::Snapshot s2 = MakeLiveSnap("Loada", 2, t0 + base::Seconds(2),
                                 /*in_combat=*/true);
  s2.base.where.in_instance = true;
  s2.base.where.map = "Magisters' Terrace";
  AddKeep(&s2);
  loop.OnSnapshot(s2, t0 + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("in Flame Strike -> out of it"),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find(
                "Phoenix appears -> target nearest Phoenix"),
            std::string::npos);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: in Flame Strike -> out of it");

  // Explicit `orders: none` clears orders even in combat.
  pb::ImmediateEvent wh2;
  wh2.bot = "Loada";
  wh2.event.chat.emplace().channel = pb::WHISPER;
  wh2.event.chat->from = "Friend";
  wh2.event.chat->text = "clear";
  loop.OnEvent(std::move(wh2), t0 + base::Seconds(3));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60\norders: none");

  // Next wake in combat should have no active standing orders.
  pb::ImmediateEvent wh3;
  wh3.bot = "Loada";
  wh3.event.chat.emplace().channel = pb::WHISPER;
  wh3.event.chat->from = "Friend";
  wh3.event.chat->text = "check";
  loop.OnEvent(std::move(wh3), t0 + base::Seconds(4));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_EQ(brain.front_request().user.find("Active standing orders:"),
            std::string::npos);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // Re-entering the instance restores the original 2-order pre-pull cache,
  // NOT the 1-order mid-fight plan.
  loop.OnConnectionChanged(false);
  loop.OnConnectionChanged(true);
  pb::Snapshot s3 = MakeLiveSnap("Loada", 3, t0 + base::Seconds(5),
                                 /*in_combat=*/false);
  s3.base.where.in_instance = true;
  s3.base.where.map = "Magisters' Terrace";
  AddKeep(&s3);
  loop.OnSnapshot(s3, t0 + base::Seconds(5));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("in Flame Strike -> out of it"),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find(
                "Phoenix appears -> target nearest Phoenix"),
            std::string::npos);

  std::filesystem::remove_all(tmp_dir);
}

TEST(BotLoopTest, AutoLoadsInstanceNotesFromNotesDirOnInstanceEntry) {
  const std::filesystem::path tmp_dir =
      std::filesystem::temp_directory_path() / "wlb_bot_loop_notes_dir_test";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir / "notes" / "ICC");
  std::filesystem::create_directories(tmp_dir / "bots");
  {
    std::ofstream f(tmp_dir / "notes" / "ICC" / "notes.md");
    f << "# Icecrown Citadel (ICC)\n\n"
      << "## The Lich King\n"
      << "- Defile: move out immediately.\n"
      << "```\nin Defile -> out of it\n```\n";
  }

  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  // Real play always has a non-empty `bots_dir` (default `var/bots`) with no
  // per-bot `notes.md`; `StartBotCall` must not wipe auto-loaded instance
  // notes.
  cfg.bots_dir = (tmp_dir / "bots").string();
  cfg.notes_dir = (tmp_dir / "notes").string();
  cfg.stale_snapshot_after = base::Seconds(3600);
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot snap = MakeLiveSnap("Loada", 1, t0, /*in_combat=*/false);
  snap.base.where.in_instance = true;
  snap.base.where.map = "Icecrown Citadel";
  AddKeep(&snap);
  loop.OnSnapshot(snap, t0);

  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("compile standing orders"),
            std::string::npos);
  EXPECT_NE(brain.front_request().user.find("in Defile -> out of it"),
            std::string::npos);

  std::filesystem::remove_all(tmp_dir);
}

TEST(BotLoopTest, TellGrammarIncludesOnlyWhisperersAndPartyMembers) {
  CannedBrain brain;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(), [](const pb::Command&) {});

  const base::TimeTicks t0 = base::TimeTicks::Now();
  pb::Snapshot s1 = MakeLiveSnap("Gatda", 1, t0, /*in_combat=*/false);
  AddKeep(&s1);
  pb::Unit mate;
  mate.name = "Partymate";
  s1.ext.party.emplace().members.push_back(mate);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // Human whispers Gatda.
  pb::ImmediateEvent wh;
  wh.bot = "Gatda";
  wh.event.chat.emplace().channel = pb::WHISPER;
  wh.event.chat->from = "Humanpal";
  wh.event.chat->text = "hey";
  loop.OnEvent(std::move(wh), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");

  // Five General/Say/Invite events from strangers must NOT evict Humanpal or
  // allow whispering those strangers.
  for (int i = 0; i < 5; ++i) {
    pb::ImmediateEvent ev;
    ev.bot = "Gatda";
    ev.event.chat.emplace().channel = (i % 2 == 0) ? pb::GENERAL : pb::SAY;
    ev.event.chat->from = "Stranger" + std::to_string(i);
    ev.event.chat->text = "lfg";
    loop.OnEvent(std::move(ev), t0 + base::Seconds(2 + i));
  }
  pb::ImmediateEvent inv;
  inv.bot = "Gatda";
  inv.event.prompt.emplace().kind = pb::PromptEvent::GROUP_INVITE;
  inv.event.prompt->from = "Inviter";
  loop.OnEvent(std::move(inv), t0 + base::Seconds(8));
  ASSERT_EQ(brain.size(), 1u);
  const std::string& grammar = brain.front_request().grammar;
  EXPECT_NE(grammar.find("\"Humanpal\""), std::string::npos);
  EXPECT_NE(grammar.find("\"Partymate\""), std::string::npos);
  EXPECT_EQ(grammar.find("\"Stranger0\""), std::string::npos);
  EXPECT_EQ(grammar.find("\"Inviter\""), std::string::npos);
}

TEST(BotLoopTest, LeavingInstanceClearsOrdersAndSelfCancelDoesNotWake) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });
  const base::TimeTicks t0 = base::TimeTicks::Now();

  // Bot inside ICC gets a standing order and triggers it.
  pb::Snapshot s1 = MakeLiveSnap("Gatda", 1, t0, /*in_combat=*/false);
  AddKeep(&s1);
  s1.base.where.in_instance = true;
  s1.base.where.map = "Icecrown Citadel";
  pb::Area& fire = s1.base.nearby.areas.emplace_back();
  fire.guid = "0xa1";
  fire.name = "Coldflame";
  fire.hostile = true;
  fire.inside_me = false;
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: in Coldflame -> out of it");

  // Trigger Coldflame so an act is dispatched.
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  s2.base.nearby.areas[0].inside_me = true;
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_FALSE(sent.empty());
  const uint32_t act_cmd_id = sent.back().command_id;
  ASSERT_TRUE(sent.back().act.has_value());

  // M7: "act cancelled by new choice" arriving with !call_in_flight must NOT
  // wake the brain urgently.
  pb::ImmediateEvent cancel_ev;
  cancel_ev.bot = "Gatda";
  cancel_ev.event.error.emplace().command_id = act_cmd_id;
  cancel_ev.event.error->text = "act cancelled by new choice";
  loop.OnEvent(std::move(cancel_ev), t0 + base::Seconds(2));
  EXPECT_EQ(brain.size(), 0u);

  // L-Act1: re-trigger Coldflame so a movement order is ACTIVE, then leave the
  // instance. Leaving must clear standing orders AND emit STOP/MOVING.
  pb::Snapshot s2b = s1;
  s2b.seq = 3;
  s2b.base.nearby.areas[0].inside_me = false;
  loop.OnSnapshot(s2b, t0 + base::Seconds(3));
  pb::Snapshot s2c = s1;
  s2c.seq = 4;
  s2c.base.nearby.areas[0].inside_me = true;
  loop.OnSnapshot(s2c, t0 + base::Seconds(4));
  ASSERT_TRUE(sent.back().act.has_value());
  EXPECT_EQ(sent.back().act->verb, pb::Command_Act::OUT_OF);

  pb::Snapshot s3 = s2c;
  s3.seq = 5;
  s3.base.where.in_instance = false;
  s3.base.where.map = "Northrend";
  const size_t cmds_before_exit = sent.size();
  loop.OnSnapshot(s3, t0 + base::Seconds(5));
  bool saw_stop_moving = false;
  for (size_t i = cmds_before_exit; i < sent.size(); ++i) {
    if (sent[i].act && sent[i].act->verb == pb::Command_Act::STOP &&
        sent[i].act->stop == pb::Command_Act::MOVING) {
      saw_stop_moving = true;
    }
  }
  EXPECT_TRUE(saw_stop_moving);

  // Post-exit snapshot with Coldflame still present must NOT dispatch any act.
  pb::Snapshot s4 = s3;
  s4.seq = 6;
  const size_t cmds_after_exit = sent.size();
  loop.OnSnapshot(s4, t0 + base::Seconds(6));
  for (size_t i = cmds_after_exit; i < sent.size(); ++i) {
    EXPECT_FALSE(sent[i].act.has_value());
  }
}

TEST(BotLoopTest, ChatErrorWakesBrainWithoutStoppingActiveRunner) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "RULES";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });
  const base::TimeTicks t0 = base::TimeTicks::Now();

  pb::Snapshot s1 = MakeLiveSnap("Gatda", 1, t0, /*in_combat=*/false);
  pb::Option& attack = s1.options.emplace_back();
  attack.kind = pb::Option::ATTACK;
  attack.text = "Attack Boar";
  attack.guid = "0xb1";
  attack.name = "Boar";
  pb::Unit& boar = s1.base.nearby.units.emplace_back();
  boar.guid = "0xb1";
  boar.name = "Boar";
  boar.kind = pb::Unit::NPC;
  boar.reaction = pb::Unit::HOSTILE;
  boar.health_pct = 100.0f;
  AddKeep(&s1);

  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1, 2\nexpect: none\nwatch: none\nlook: 60\n"
      "say: pulling boar\ntell Ally: incoming");
  uint32_t attack_cmd_id = 0;
  uint32_t say_cmd_id = 0;
  uint32_t tell_cmd_id = 0;
  for (const pb::Command& c : sent) {
    if (c.choose.has_value()) {
      attack_cmd_id = c.command_id;
    }
    if (c.say.has_value()) {
      if (say_cmd_id == 0u) {
        say_cmd_id = c.command_id;
      } else {
        tell_cmd_id = c.command_id;
      }
    }
  }
  ASSERT_NE(attack_cmd_id, 0u);
  ASSERT_NE(say_cmd_id, 0u);
  ASSERT_NE(tell_cmd_id, 0u);

  // Chat error with the real say command_id wakes the brain (refusal=false)
  // without stopping the active Attack step or arming refusal backoff.
  pb::ImmediateEvent err;
  err.bot = "Gatda";
  err.event.error.emplace().command_id = say_cmd_id;
  err.event.error->text = "chat rate limited";
  loop.OnEvent(std::move(err), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("chat rate limited"),
            std::string::npos);

  // Even if the chat-error wake fails to return a new plan, refusal backoff
  // is NOT armed (a second chat error 0.5 s later still wakes immediately),
  // and state.runner stays active: completing attack_cmd_id dispatches step 2.
  brain.FinishError("timeout");
  pb::ImmediateEvent err2;
  err2.bot = "Gatda";
  err2.event.error.emplace().command_id = tell_cmd_id;
  err2.event.error->text = "chat blocked";
  loop.OnEvent(std::move(err2), t0 + base::Milliseconds(1500));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("chat blocked"), std::string::npos);
  brain.FinishError("timeout");
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  s2.done_command_id = attack_cmd_id;
  s2.base.nearby.units[0].dead = true;
  const size_t cmds_before_step2 = sent.size();
  loop.OnSnapshot(s2, t0 + base::Seconds(2));
  bool dispatched_step2 = false;
  for (size_t i = cmds_before_step2; i < sent.size(); ++i) {
    if (sent[i].choose && sent[i].choose->option == 2u) {
      dispatched_step2 = true;
    }
  }
  EXPECT_TRUE(dispatched_step2);
}

TEST(BotLoopTest, LastOutcomeDeliveredOnceAndKnowledgeInjected) {
  namespace fs = std::filesystem;
  struct ScopedTempFile {
    fs::path path;
    ~ScopedTempFile() {
      std::error_code ec;
      fs::remove(path, ec);
    }
  };
  ScopedTempFile tmp{fs::temp_directory_path() /
                     ("wlb_bot_loop_knowledge_" +
                      std::to_string(static_cast<long>(::getpid())) + ".json")};
  {
    std::ofstream out(tmp.path);
    out << R"json({
      "version": 1,
      "quests": {},
      "zones": {
        "Elwynn Forest": {
          "min_level": 1,
          "max_level": 10,
          "faction": "Alliance",
          "next_zones": ["Westfall (10-20)"]
        }
      },
      "dungeons": {}
    })json";
  }

  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "rules";
  cfg.knowledge_path = tmp.path.string();
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& cmd) { sent.push_back(cmd); });

  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot s1 = MakeLiveSnap("Gatda", 1, t0);
  s1.base.self.unit.level = 14;
  s1.base.where.zone = "Elwynn Forest";
  pb::Option& atk = s1.options.emplace_back();
  atk.text = "Attack Boar";
  atk.kind = pb::Option::ATTACK;
  atk.guid = "0xb1";
  atk.name = "Boar";
  AddKeep(&s1);

  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Outleveled zone: you are level 14"),
      std::string::npos);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  uint32_t attack_cmd_id = 0;
  for (const pb::Command& c : sent) {
    if (c.choose) {
      attack_cmd_id = c.command_id;
    }
  }
  ASSERT_NE(attack_cmd_id, 0u);

  // Refusal populates `Last outcome:` on the immediately following wake.
  pb::ImmediateEvent err;
  err.bot = "Gatda";
  err.event.error.emplace().command_id = attack_cmd_id;
  err.event.error->text = "target out of range";
  loop.OnEvent(std::move(err), t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Last outcome: target out of range."),
      std::string::npos);

  // A newer wake arriving while the call is in flight supersedes the reply and
  // requeues `Last outcome:` for the queued wake.
  pb::ImmediateEvent w_super;
  w_super.bot = "Gatda";
  w_super.event.chat.emplace().channel = pb::WHISPER;
  w_super.event.chat->from = "Ally";
  w_super.event.chat->text = "hold on";
  loop.OnEvent(std::move(w_super), t0 + base::Seconds(2));
  brain.FinishReply("do: 2\nexpect: none\nwatch: none\nlook: 60");
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Last outcome: target out of range."),
      std::string::npos);

  // Same-epoch cancellation also requeues `Last outcome:` for the next wake.
  brain.FinishCancelled();
  pb::ImmediateEvent w_cancel;
  w_cancel.bot = "Gatda";
  w_cancel.event.chat.emplace().channel = pb::WHISPER;
  w_cancel.event.chat->from = "Ally";
  w_cancel.event.chat->text = "still there?";
  loop.OnEvent(std::move(w_cancel), t0 + base::Seconds(3));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Last outcome: target out of range."),
      std::string::npos);

  // A transport/brain error also requeues `Last outcome:` for the retry wake.
  brain.FinishError("timeout");
  pb::ImmediateEvent w_err;
  w_err.bot = "Gatda";
  w_err.event.chat.emplace().channel = pb::WHISPER;
  w_err.event.chat->from = "Ally";
  w_err.event.chat->text = "retry";
  loop.OnEvent(std::move(w_err), t0 + base::Seconds(4));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Last outcome: target out of range."),
      std::string::npos);

  // An unparseable plan requeues `Last outcome:` for the retry wake.
  brain.FinishReply("garbage not a valid plan");
  pb::ImmediateEvent w0;
  w0.bot = "Gatda";
  w0.event.chat.emplace().channel = pb::WHISPER;
  w0.event.chat->from = "Ally";
  w0.event.chat->text = "ping";
  loop.OnEvent(std::move(w0), t0 + base::Seconds(5));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(
      brain.front_request().user.find("Last outcome: target out of range."),
      std::string::npos);

  // A valid Keep reply consumes `Last outcome:` so the next wake does NOT
  // repeat it.
  brain.FinishReply("do: 2\nexpect: none\nwatch: none\nlook: 60");
  pb::ImmediateEvent whisper;
  whisper.bot = "Gatda";
  whisper.event.chat.emplace().channel = pb::WHISPER;
  whisper.event.chat->from = "Ally";
  whisper.event.chat->text = "ready?";
  loop.OnEvent(std::move(whisper), t0 + base::Seconds(6));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_EQ(brain.front_request().user.find("Last outcome:"),
            std::string::npos);

  // Trigger a fresh refusal so an in-flight call actually carries a non-empty
  // `asked_outcome`, then disconnect before it cancels: the late cancelled
  // reply from the old epoch must NOT resurrect the stale outcome.
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  uint32_t attack_cmd_id_2 = 0;
  for (const pb::Command& c : sent) {
    if (c.choose) {
      attack_cmd_id_2 = c.command_id;
    }
  }
  ASSERT_NE(attack_cmd_id_2, attack_cmd_id);
  pb::ImmediateEvent err2;
  err2.bot = "Gatda";
  err2.event.error.emplace().command_id = attack_cmd_id_2;
  err2.event.error->text = "not enough mana";
  loop.OnEvent(std::move(err2), t0 + base::Seconds(7));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("Last outcome: not enough mana."),
            std::string::npos);

  loop.OnConnectionChanged(false);
  brain.FinishCancelled();
  loop.OnConnectionChanged(true);
  loop.OnSnapshot(s1, t0 + base::Seconds(8));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_EQ(brain.front_request().user.find("Last outcome:"),
            std::string::npos);
}

TEST(BotLoopTest, OrderWakeAndTriggerCoalesceIntoSingleCall) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "rules";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);

  // First snapshot installs a standing order that wakes on `Fire`.
  pb::Snapshot s1 = MakeLiveSnap("Gatda", 1, t0);
  s1.base.self.in_combat = true;
  s1.base.self.unit.health_pct = 100;
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\norder: in Fire -> wake\nexpect: none\nwatch: none\nlook: 60");
  ASSERT_EQ(brain.size(), 0u);

  // Next snapshot triggers BOTH the standing-order wake (`in Fire`) AND an
  // uncovered snapshot trigger (stunned). BotLoop must coalesce both into a
  // single LLM call carrying both reasons (no double-wake).
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  s2.base.self.control.stunned = true;
  pb::Area fire;
  fire.guid = "area:fire";
  fire.name = "Fire";
  fire.inside_me = true;
  fire.hostile = true;
  s2.base.nearby.areas.push_back(fire);
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("in Fire"), std::string::npos);
  EXPECT_NE(brain.front_request().user.find("lost control (stunned)"),
            std::string::npos);
  brain.FinishReply("do: 1\nexpect: none\nwatch: none\nlook: 60");
  // No second call queued for the same snapshot.
  EXPECT_EQ(brain.size(), 0u);
}

TEST(BotLoopTest, EdgeTriggerNotDroppedDuringOrderRefusalBackoff) {
  CannedBrain brain;
  std::vector<pb::Command> sent;
  BotLoopConfig cfg;
  cfg.rules = "rules";
  BotLoop loop(cfg, brain.Fn(),
               [&](const pb::Command& c) { sent.push_back(c); });
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);

  // Install a standing order `in Fire -> out of it`.
  pb::Snapshot s1 = MakeLiveSnap("Gatda", 1, t0);
  s1.base.self.in_combat = true;
  s1.base.self.unit.health_pct = 100;
  AddKeep(&s1);
  loop.OnSnapshot(s1, t0);
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\norder: in Fire -> out of it\nexpect: none\nwatch: none\nlook: "
      "60");

  // Trigger the order -> sends act command_id.
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  pb::Area fire;
  fire.guid = "area:fire";
  fire.name = "Fire";
  fire.inside_me = true;
  fire.hostile = true;
  s2.base.nearby.areas.push_back(fire);
  loop.OnSnapshot(s2, t0 + base::Seconds(1));
  uint32_t act_id1 = 0;
  for (const pb::Command& c : sent) {
    if (c.act) {
      act_id1 = c.command_id;
    }
  }
  ASSERT_NE(act_id1, 0u);

  // First failure wakes immediately (order_wakes == 1, backoff == 0).
  pb::ImmediateEvent err1;
  err1.bot = "Gatda";
  err1.event.error.emplace().command_id = act_id1;
  err1.event.error->text = "no path";
  loop.OnEvent(std::move(err1), t0 + base::Seconds(2));
  ASSERT_EQ(brain.size(), 1u);
  brain.FinishReply(
      "do: 1\norder: in Fire -> out of it\nexpect: none\nwatch: none\nlook: "
      "60");

  // Fire again to arm a second act.
  pb::Snapshot s3 = s2;
  s3.seq = 3;
  loop.OnSnapshot(s3, t0 + base::Seconds(3));
  ASSERT_EQ(brain.size(), 0u);

  // Let the second act time out (order_wakes == 2 -> backoff 2 s, deferred)
  // on the exact same snapshot where an uncovered edge trigger (stunned) fires.
  pb::Snapshot s4 = s2;
  s4.seq = 4;
  s4.base.self.control.stunned = true;
  loop.OnSnapshot(s4, t0 + base::Seconds(30));
  // Edge trigger must wake the bot instead of being dropped during backoff.
  ASSERT_EQ(brain.size(), 1u);
  EXPECT_NE(brain.front_request().user.find("lost control (stunned)"),
            std::string::npos);
}

}  // namespace
}  // namespace harness
