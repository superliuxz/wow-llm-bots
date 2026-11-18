// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_scripts.h"

#include <string>
#include <utility>

#include "Chat.h"
#include "ChatCommand.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "Common.h"
#include "Define.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "base/memory/raw_ptr.h"
#include "chat/chat_inbox.h"
#include "chat/speak.h"
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
  explicit LlmBrainChatScript(ChatInbox* inbox)
      : PlayerScript("LlmBrainChatScript",
                     {PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT}),
        inbox_(inbox) {}

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
    inbox_->Add(receiver->GetName(), std::move(chat));
    return true;  // Only listening: the whisper goes through.
  }

 private:
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

void AddLlmBrainChatScripts(ChatInbox* inbox) {
  new LlmBrainChatScript(inbox);
  new LlmBrainCommandScript();
}

}  // namespace llm_brain
