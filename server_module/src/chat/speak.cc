// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/speak.h"

#include <cstdint>
#include <string>

#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "Common.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "chat/chat_text.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// GM silence (the server's chat handler checks it by id).
constexpr uint32_t kGmSilenceAura = 1852;

// The server's checks on a whisper from `bot` to `to` that a player's client
// would hit (WorldSession::HandleMessagechatOpcode, CHAT_MSG_WHISPER), in its
// order; empty when it may be sent.
std::string WhyCantWhisper(Player* bot, Player* to, const std::string& name) {
  const auto level_req = static_cast<uint32_t>(
      sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ));
  if (bot->GetLevel() < level_req && !to->IsGameMaster()) {
    return "you must be level " + std::to_string(level_req) + " to whisper";
  }
  WorldSession* session = to->GetSession();
  if (!to->isAcceptWhispers() && session &&
      session->HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) &&
      !to->IsInWhisperWhiteList(bot->GetGUID())) {
    // The client says the same as for nobody there.
    return "no player named '" + name + "' is currently playing";
  }
  const bool bot_is_player =
      !bot->GetSession() || !bot->GetSession()->HasPermission(
                                rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT);
  const bool to_is_player =
      !session ||
      !session->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT);
  if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT) &&
      bot_is_player && to_is_player && bot->GetTeamId() != to->GetTeamId()) {
    return "you can't whisper players of the other faction";
  }
  if (bot->HasAura(kGmSilenceAura) && !to->IsGameMaster()) {
    return "you have been silenced";
  }
  return "";
}

// The server's checks before any say, yell or whisper a player's client sends
// (HandleMessagechatOpcode): the first-login mute (most chat types), and for
// out loud, a spectator or a dead body (dropped silently by the server) and
// the level required to talk (review of c80e791, finding 14, the part left).
std::string WhyCantTalk(Player* bot, bool out_loud) {
  WorldSession* session = bot->GetSession();
  if (sWorld->getBoolConfig(CONFIG_CHAT_MUTE_FIRST_LOGIN) &&
      !(session &&
        session->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_CHAT_CHANNEL_REQ))) {
    const uint32_t minutes =
        sWorld->getIntConfig(CONFIG_CHAT_TIME_MUTE_FIRST_LOGIN);
    if (bot->GetTotalPlayedTime() < minutes * MINUTE) {
      return "you can't talk until you have played " + std::to_string(minutes) +
             " minutes";
    }
  }
  if (!out_loud) {
    return "";
  }
  if (bot->IsSpectator()) {
    return "spectators can't talk";
  }
  if (!bot->IsAlive()) {
    return "you are dead";
  }
  const auto level_req =
      static_cast<uint32_t>(sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ));
  if (bot->GetLevel() < level_req) {
    return "you must be level " + std::to_string(level_req) +
           " to talk out loud";
  }
  return "";
}

}  // namespace

std::string Speak(Player* bot, const pb::Command::Say& say) {
  const std::string text = CleanChatText(say.text);
  if (text.empty()) {
    return "nothing to say";
  }
  if (!bot->CanSpeak()) {
    return "you can't speak yet (muted)";
  }
  if (std::string why = WhyCantTalk(bot, say.channel != pb::WHISPER);
      !why.empty()) {
    return why;
  }
  // Out loud a player speaks their race's language; whispers are always
  // readable (Player::Whisper makes them so).
  const Language language =
      bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
  switch (say.channel) {
    case pb::SAY:
      bot->Say(text, language);
      return "";
    case pb::YELL:
      bot->Yell(text, language);
      return "";
    case pb::PARTY: {
      Group* group = bot->GetGroup();
      if (!group) {
        return "you are not in a party";
      }
      WorldPacket data;
      const ChatMsg msg_type = group->IsLeader(bot->GetGUID())
                                   ? CHAT_MSG_PARTY_LEADER
                                   : CHAT_MSG_PARTY;
      ChatHandler::BuildChatPacket(data, msg_type, language, bot, bot, text);
      group->BroadcastPacket(&data, false,
                             group->GetMemberGroup(bot->GetGUID()));
      return "";
    }
    case pb::RAID: {
      Group* group = bot->GetGroup();
      if (!group || !group->isRaidGroup()) {
        return "you are not in a raid";
      }
      WorldPacket data;
      const ChatMsg msg_type = group->IsLeader(bot->GetGUID())
                                   ? CHAT_MSG_RAID_LEADER
                                   : CHAT_MSG_RAID;
      ChatHandler::BuildChatPacket(data, msg_type, language, bot, bot, text);
      group->BroadcastPacket(&data, false);
      return "";
    }
    case pb::GENERAL: {
      ChannelMgr* cmgr = ChannelMgr::forTeam(bot->GetTeamId());
      if (!cmgr) {
        return "no channel manager";
      }
      std::string zone_name;
      if (const AreaTableEntry* area =
              sAreaTableStore.LookupEntry(bot->GetZoneId())) {
        const char* zn = area->area_name[sWorld->GetDefaultDbcLocale()];
        if (!zn || zn[0] == '\0') {
          zn = area->area_name[0];
        }
        if (zn) {
          zone_name = zn;
        }
      }
      const std::string cname =
          zone_name.empty() ? "General" : "General - " + zone_name;
      Channel* chn = cmgr->GetJoinChannel(cname, 1);
      if (!chn) {
        return "cannot join " + cname;
      }
      chn->JoinChannel(bot, "");
      chn->Say(bot->GetGUID(), text, language);
      return "";
    }
    case pb::WHISPER: {
      std::string name = say.to;
      Player* to = normalizePlayerName(name)
                       ? ObjectAccessor::FindPlayerByName(name)
                       : nullptr;
      if (to == nullptr || !to->IsInWorld()) {
        return "no player named '" + say.to + "' is currently playing";
      }
      if (to == bot) {
        return "you can't whisper yourself";
      }
      if (std::string why = WhyCantWhisper(bot, to, say.to); !why.empty()) {
        return why;
      }
      bot->Whisper(text, LANG_UNIVERSAL, to);
      return "";
    }
    default:
      return "you can't talk on " + std::string(pb::EnumName(say.channel)) +
             " yet";
  }
}

}  // namespace llm_brain
