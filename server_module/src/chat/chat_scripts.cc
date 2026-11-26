// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_scripts.h"

#include <algorithm>  // IWYU pragma: keep (LLM_BRAIN_TEST_HOOKS)
#include <atomic>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ByteBuffer.h"
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
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerScript.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/memory/raw_ptr.h"
#include "chat/chat_inbox.h"
#include "chat/chat_text.h"
#include "chat/speak.h"
#include "snapshot/behavior_trace.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;
using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;
using Acore::ChatCommands::Tail;

#if defined(LLM_BRAIN_TEST_HOOKS)
// Names whose lines count as a human's (SetTestHuman; test builds only).
std::vector<std::string>& TestHumans() {
  static auto* names = new std::vector<std::string>();
  return *names;
}
#endif

}  // namespace

bool IsHumanPlayer(Player* player) {
  if (GET_PLAYERBOT_AI(player) == nullptr) {
    return true;
  }
#if defined(LLM_BRAIN_TEST_HOOKS)
  const std::vector<std::string>& names = TestHumans();
  return std::find(names.begin(), names.end(), player->GetName()) !=
         names.end();
#else
  return false;
#endif
}

namespace {

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
    const bool sender_is_human = IsHumanPlayer(player);
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
      chat.at_me =
          MentionsName(msg, bot_name) ||
          (!is_yell && sender_is_human && dist <= 15.0f && !IsTrivialChat(msg));
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
        type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER &&
        type != CHAT_MSG_RAID_WARNING) {
      return true;
    }
    const bool is_warning = type == CHAT_MSG_RAID_WARNING;  // T10.
    const bool is_raid =
        (type == CHAT_MSG_RAID || type == CHAT_MSG_RAID_LEADER);
    // In a raid, `/p` only reaches the sender's subgroup (T13).
    const bool subgroup_only = !is_raid && !is_warning && group->isRaidGroup();
    const bool sender_is_human = IsHumanPlayer(player);
    for (const std::string& bot_name : inbox_->Bots()) {
      if (bot_name == player->GetName()) {
        continue;
      }
      Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
      if (!bot || !bot->IsInWorld() || bot->GetGroup() != group) {
        continue;
      }
      if (subgroup_only && bot->GetSubGroup() != player->GetSubGroup()) {
        continue;
      }
      pb::ChatEvent chat;
      chat.channel =
          is_warning ? pb::RAID_WARNING : (is_raid ? pb::RAID : pb::PARTY);
      chat.from = player->GetName();
      chat.text = msg;
      // Only human party/raid messages wake bots directly; bot-to-bot party
      // lines remain visible in the chat log without triggering reply loops.
      // "ok" / "lol" asks nothing of anyone: heard, not woken for (RH-H4).
      // The harness (`BotLoop::WakesForPartyLine`) filters party and raid lines
      // to addressed bots (by name, class, or role) or a single responder.
      chat.at_me = is_warning || (sender_is_human && !IsTrivialChat(msg));
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
    // Only forward constant public channels (General / Trade /
    // LookingForGroup); never leak custom/private channels or channels the bot
    // has not joined. LookingForGroup is where pugs formed on 3.3.5a (RH-M7).
    if (!channel->IsConstant() || channel->HasFlag(CHANNEL_FLAG_CUSTOM)) {
      return true;
    }
    const ChatChannelsEntry* ch_entry =
        sChatChannelsStore.LookupEntry(channel->GetChannelId());
    if (!ch_entry) {
      return true;
    }
    const std::string& cname = channel->GetName();
    pb::ChatChannel ch = pb::CUSTOM_CHANNEL;
    if (channel->IsLFG()) {
      ch = pb::LFG;
    } else if (cname.starts_with("General")) {
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
  const base::raw_ptr<ChatInbox> inbox_;
  const base::raw_ptr<std::atomic<BehaviorTraceWriter*>> human_trace_;
};

// Creature talk a bot's client receives (boss emotes, monster yells and
// says, T10): AzerothCore sends it as SMSG_MESSAGECHAT with no player chat
// hook, so it is read off the bot's outgoing packets. Runs on the map thread
// that sends it, so it only hands lines to the thread-safe ChatInbox.
class LlmBrainMonsterChatScript final : public ServerScript {
 public:
  explicit LlmBrainMonsterChatScript(ChatInbox* inbox)
      : ServerScript("LlmBrainMonsterChatScript", {SERVERHOOK_ON_PACKET_SENT}),
        inbox_(inbox) {}

  void OnPacketSent(WorldSession* session, WorldPacket const& packet) override {
    if (!session || packet.GetOpcode() != SMSG_MESSAGECHAT) {
      return;
    }
    Player* bot = session->GetPlayer();
    if (!bot || !inbox_->Contains(bot->GetName())) {
      return;
    }
    std::optional<pb::ChatEvent> chat = ParseMonsterChat(packet);
    if (chat) {
      chat->at_me = chat->channel == pb::BOSS_EMOTE ||
                    chat->channel == pb::MONSTER_WHISPER ||
                    MentionsName(chat->text, bot->GetName());
      inbox_->Add(bot->GetName(), std::move(*chat));
    }
  }

 private:
  // The ChatHandler::BuildChatPacket layout for creature senders.
  static std::optional<pb::ChatEvent> ParseMonsterChat(
      WorldPacket const& packet) {
    if (packet.size() < 1 + 4 + 8 + 4 + 4) {
      return std::nullopt;
    }
    ByteBuffer in(packet);
    in.rpos(0);
    uint8_t type = 0;
    int32_t language = 0;
    ObjectGuid sender;
    uint32_t flags = 0;
    in >> type >> language >> sender >> flags;
    pb::ChatChannel channel = pb::CHAT_CHANNEL_UNSPECIFIED;
    switch (type) {
      case CHAT_MSG_MONSTER_SAY:
        channel = pb::MONSTER_SAY;
        break;
      case CHAT_MSG_MONSTER_YELL:
        channel = pb::MONSTER_YELL;
        break;
      case CHAT_MSG_MONSTER_EMOTE:
        channel = pb::MONSTER_EMOTE;
        break;
      case CHAT_MSG_MONSTER_WHISPER:
      case CHAT_MSG_RAID_BOSS_WHISPER:
        channel = pb::MONSTER_WHISPER;
        break;
      case CHAT_MSG_RAID_BOSS_EMOTE:
        channel = pb::BOSS_EMOTE;
        break;
      default:
        return std::nullopt;
    }
    if (sender.IsPlayer()) {
      return std::nullopt;
    }
    try {
      uint32_t name_len = 0;
      in >> name_len;
      std::string name;
      in >> name;
      ObjectGuid receiver;
      in >> receiver;
      if (receiver && !receiver.IsPlayer() && !receiver.IsPet()) {
        uint32_t receiver_len = 0;
        std::string receiver_name;
        in >> receiver_len >> receiver_name;
      }
      uint32_t text_len = 0;
      std::string text;
      in >> text_len >> text;
      if (text.empty()) {
        return std::nullopt;
      }
      pb::ChatEvent chat;
      chat.channel = channel;
      chat.from = std::move(name);
      chat.text = std::move(text);
      return chat;
    } catch (const ByteBufferException&) {
      return std::nullopt;
    }
  }

  const base::raw_ptr<ChatInbox> inbox_;
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
    const std::string error = Speak(sender, say, /*allow_non_bot=*/true);
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
  new LlmBrainMonsterChatScript(inbox);
  new LlmBrainCommandScript();
}

void SetTestHuman(const std::string& name, bool human) {
#if defined(LLM_BRAIN_TEST_HOOKS)
  std::vector<std::string>& names = TestHumans();
  std::erase(names, name);
  if (human) {
    names.push_back(name);
  }
#else
  (void)name;
  (void)human;
#endif
}

}  // namespace llm_brain
