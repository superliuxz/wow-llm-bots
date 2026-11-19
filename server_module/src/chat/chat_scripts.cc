// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_scripts.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "Chat.h"
#include "ChatCommand.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "Common.h"
#include "Define.h"
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

// Whispers to the bots. Runs on the sender's thread (a map update thread for
// playerbots), so it only hands the line to the inbox.
class LlmBrainChatScript : public PlayerScript {
 public:
  LlmBrainChatScript(ChatInbox* inbox,
                     std::atomic<BehaviorTraceWriter*>* human_trace)
      : PlayerScript("LlmBrainChatScript",
                     {PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT}),
        inbox_(inbox),
        human_trace_(human_trace) {}

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
      if (!GET_PLAYERBOT_AI(player)) {
        trace->WriteChat({t_ms, mono_us, "human", player->GetName(), "out",
                          "whisper", receiver->GetName(), msg});
      }
      if (!GET_PLAYERBOT_AI(receiver)) {
        trace->WriteChat({t_ms, mono_us, "human", receiver->GetName(), "in",
                          "whisper", player->GetName(), msg});
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
