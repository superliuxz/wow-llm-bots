// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_scripts.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Cell.h"
#include "Channel.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "Common.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "GridNotifiers.h"
#include "Group.h"
#include "Language.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "chat/chat_inbox.h"
#include "chat/speak.h"
#include "snapshot/behavior_trace.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;
using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;
using Acore::ChatCommands::Tail;

// Whispers, Say/Yell, Party/Raid, and General/Trade/LFG channel chat heard by
// brain-run bots. Runs on the sender's thread, so it only hands lines to the
// thread-safe ChatInbox.
class LlmBrainChatScript : public PlayerScript {
 public:
  LlmBrainChatScript(ChatInbox* inbox,
                     std::atomic<BehaviorTraceWriter*>* human_trace)
      : PlayerScript("LlmBrainChatScript",
                     {PLAYERHOOK_CAN_PLAYER_USE_CHAT,
                      PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
                      PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
                      PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT}),
        inbox_(inbox),
        human_trace_(human_trace) {}

  bool OnPlayerCanUseChat(Player* player,
                          uint32 type,
                          uint32 language,
                          std::string& msg) override {
    if (!player || language == LANG_ADDON || msg.empty()) {
      return true;
    }
    if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL) {
      return true;
    }
    const bool is_yell = (type == CHAT_MSG_YELL);
    const float max_dist =
        is_yell ? sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL)
                : sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY);
    const bool cross_faction =
        sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT);
    const bool sender_is_human = GET_PLAYERBOT_AI(player) == nullptr;
    // Deliver Say/Yell via the sender's map grid (same map, instance, phase,
    // and thread) like upstream Player::Say/Yell — never cross-instance or
    // touching Player* owned by other map threads.
    std::list<Player*> nearby;
    Acore::AnyPlayerInObjectRangeCheck check(player, max_dist, false);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(
        player, nearby, check);
    Cell::VisitObjects(player, searcher, max_dist);
    for (Player* bot : nearby) {
      if (!bot || bot == player || !bot->IsInWorld()) {
        continue;
      }
      if (bot->GetInstanceId() != player->GetInstanceId() ||
          !bot->InSamePhase(player)) {
        continue;
      }
      const std::string& bot_name = bot->GetName();
      if (!inbox_->Contains(bot_name)) {
        continue;
      }
      if (!cross_faction && bot->GetTeamId() != player->GetTeamId()) {
        continue;
      }
      const float dist = bot->GetDistance(player);
      if (dist > max_dist) {
        continue;
      }
      pb::ChatEvent chat;
      chat.channel = is_yell ? pb::YELL : pb::SAY;
      chat.from = player->GetName();
      chat.text = msg;
      chat.from_dist_yd = dist;
      chat.at_me = MentionsName(msg, bot_name) ||
                   (!is_yell && sender_is_human && dist <= 15.0f);
      inbox_->Add(bot_name, std::move(chat));
    }
    return true;
  }

  bool OnPlayerCanUseChat(Player* player,
                          uint32 type,
                          uint32 language,
                          std::string& msg,
                          Group* group) override {
    if (!player || !group || language == LANG_ADDON || msg.empty()) {
      return true;
    }
    if (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER &&
        type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER) {
      return true;
    }
    const bool is_raid =
        (type == CHAT_MSG_RAID || type == CHAT_MSG_RAID_LEADER);
    for (const std::string& bot_name : inbox_->Bots()) {
      if (bot_name == player->GetName()) {
        continue;
      }
      Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
      if (!bot || !bot->IsInWorld() || bot->GetGroup() != group) {
        continue;
      }
      pb::ChatEvent chat;
      chat.channel = is_raid ? pb::RAID : pb::PARTY;
      chat.from = player->GetName();
      chat.text = msg;
      chat.at_me = !is_raid || MentionsName(msg, bot_name);
      inbox_->Add(bot_name, std::move(chat));
    }
    return true;
  }

  bool OnPlayerCanUseChat(Player* player,
                          uint32 type,
                          uint32 language,
                          std::string& msg,
                          Channel* channel) override {
    if (!player || !channel || type != CHAT_MSG_CHANNEL ||
        language == LANG_ADDON || msg.empty()) {
      return true;
    }
    // Only forward constant public channels (General / Trade); never leak
    // custom/private channels or channels the bot has not joined.
    if (!channel->IsConstant() || channel->HasFlag(CHANNEL_FLAG_CUSTOM) ||
        channel->IsLFG()) {
      return true;
    }
    const ChatChannelsEntry* ch_entry =
        sChatChannelsStore.LookupEntry(channel->GetChannelId());
    if (!ch_entry) {
      return true;
    }
    const std::string& cname = channel->GetName();
    pb::ChatChannel ch = pb::CUSTOM_CHANNEL;
    if (cname.starts_with("General")) {
      ch = pb::GENERAL;
    } else if (channel->HasFlag(CHANNEL_FLAG_TRADE) ||
               cname.starts_with("Trade")) {
      ch = pb::TRADE;
    } else {
      return true;
    }
    const bool cross_faction =
        sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT);
    for (const std::string& bot_name : inbox_->Bots()) {
      if (bot_name == player->GetName()) {
        continue;
      }
      Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
      if (!bot || !bot->IsInWorld()) {
        continue;
      }
      if (!cross_faction && bot->GetTeamId() != player->GetTeamId()) {
        continue;
      }
      if (ch == pb::GENERAL && bot->GetZoneId() != player->GetZoneId()) {
        continue;
      }
      const AreaTableEntry* bot_zone =
          sAreaTableStore.LookupEntry(bot->GetZoneId());
      if (!bot_zone || !bot->CanJoinConstantChannelInZone(ch_entry, bot_zone)) {
        continue;
      }
      pb::ChatEvent chat;
      chat.channel = ch;
      chat.channel_name = cname;
      chat.from = player->GetName();
      chat.text = msg;
      // Wake only when the bot is explicitly named; all other General/Trade
      // lines are queued silently for "since your last look" so one LFG call
      // never triggers a zone-wide bot wake storm.
      chat.at_me = MentionsName(msg, bot_name);
      inbox_->Add(bot_name, std::move(chat));
    }
    return true;
  }

  bool OnPlayerCanUseChat(Player* player,
                          uint32 type,
                          uint32 language,
                          std::string& msg,
                          Player* receiver) override {
    if (type != CHAT_MSG_WHISPER || language == LANG_ADDON || !player ||
        !receiver) {
      return true;
    }
    if (BehaviorTraceWriter* trace =
            human_trace_ ? human_trace_->load(std::memory_order_acquire)
                         : nullptr;
        trace && trace->enabled()) {
      const int64_t t_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      const int64_t mono_us =
          (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
      // Record timing and byte length only (never private whisper text or
      // other players' names) so human-baseline reply delay / length metrics
      // work without logging chat contents or peer identities.
      if (!GET_PLAYERBOT_AI(player)) {
        trace->WriteChat({t_ms, mono_us, "human", player->GetName(), "out",
                          "whisper", "", "", msg.size()});
      }
      if (!GET_PLAYERBOT_AI(receiver)) {
        trace->WriteChat({t_ms, mono_us, "human", receiver->GetName(), "in",
                          "whisper", "", "", msg.size()});
      }
    }
    pb::ChatEvent chat;
    chat.channel = pb::WHISPER;
    chat.from = player->GetName();
    chat.text = msg;
    chat.at_me = true;
    if (!inbox_->Add(receiver->GetName(), std::move(chat))) {
      return true;  // Not one of ours: only listening.
    }
    // One of the brain's bots: a whisper is for its brain, not a playerbots
    // command. Playerbots' handler (it runs after this one: modules register
    // in name order) checks its security before the command prefix, so a
    // plain whisper could draw a canned reply ("I'm kind of busy now"):
    // deliver it here and stop the hooks, unless it is a "!pb " command
    // (review of c80e791, finding 1, the part left after 4ce9490).
    const std::string& prefix = sPlayerbotAIConfig.CommandPrefix;
    if (prefix.empty() || msg.starts_with(prefix)) {
      return true;
    }
    DeliverWhisper(player, receiver, static_cast<Language>(language), msg);
    return false;
  }

  // What Player::Whisper does after its script hook: the line to the
  // receiver, "To ..." to the sender, the chat filter, AFK / DND replies.
  static void DeliverWhisper(Player* sender,
                             Player* receiver,
                             Language language,
                             const std::string& text) {
    const bool filtered = sWorld->getBoolConfig(CONFIG_CHAT_FILTER_WHISPER) &&
                          Player::IsChatFiltered(text);
    WorldPacket data;
    if (!filtered) {
      ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, language, sender,
                                   sender, text);
      receiver->SendDirectMessage(&data);
    }
    ChatHandler::BuildChatPacket(
        data, filtered ? CHAT_MSG_FILTERED : CHAT_MSG_WHISPER_INFORM, language,
        receiver, receiver, text);
    sender->SendDirectMessage(&data);
    if (!sender->isAcceptWhispers() && !sender->IsGameMaster() &&
        !receiver->IsGameMaster()) {
      sender->SetAcceptWhispers(true);
      ChatHandler(sender->GetSession()).SendSysMessage(LANG_COMMAND_WHISPERON);
    }
    if (receiver->isAFK()) {
      ChatHandler(sender->GetSession())
          .PSendSysMessage(LANG_PLAYER_AFK, receiver->GetName(),
                           receiver->autoReplyMsg);
    } else if (receiver->isDND()) {
      ChatHandler(sender->GetSession())
          .PSendSysMessage(LANG_PLAYER_DND, receiver->GetName(),
                           receiver->autoReplyMsg);
    }
  }

 private:
  static bool MentionsName(std::string_view text, std::string_view name) {
    if (name.empty() || text.size() < name.size()) {
      return false;
    }
    auto it = std::search(
        text.begin(), text.end(), name.begin(), name.end(), [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) ==
                 std::tolower(static_cast<unsigned char>(b));
        });
    return it != text.end();
  }

  const base::raw_ptr<ChatInbox> inbox_;
  const base::raw_ptr<std::atomic<BehaviorTraceWriter*>> human_trace_;
};

class LlmBrainCommandScript : public CommandScript {
 public:
  LlmBrainCommandScript() : CommandScript("LlmBrainCommandScript") {}

  ChatCommandTable GetCommands() const override {
    static ChatCommandTable llm_brain_table = {
        {"whisper", HandleWhisper, SEC_ADMINISTRATOR, Console::Yes},
    };
    static ChatCommandTable table = {
        {"llmbrain", llm_brain_table},
    };
    return table;
  }

 private:
  static bool HandleWhisper(ChatHandler* handler,
                            std::string from,
                            std::string to,
                            Tail text) {
    Player* sender = normalizePlayerName(from)
                         ? ObjectAccessor::FindPlayerByName(from)
                         : nullptr;
    if (sender == nullptr || !sender->IsInWorld()) {
      handler->SendErrorMessage("{} is not online", from);
      return false;
    }
    pb::Command::Say say;
    say.channel = pb::WHISPER;
    say.to = to;
    say.text = std::string(text);
    const std::string error = Speak(sender, say);
    if (!error.empty()) {
      handler->SendErrorMessage("{}", error);
      return false;
    }
    handler->PSendSysMessage("{} whispers to {}: {}", from, to, say.text);
    return true;
  }
};

}  // namespace

void AddLlmBrainChatScripts(ChatInbox* inbox,
                            std::atomic<BehaviorTraceWriter*>* human_trace) {
  new LlmBrainChatScript(inbox, human_trace);
  new LlmBrainCommandScript();
}

}  // namespace llm_brain
