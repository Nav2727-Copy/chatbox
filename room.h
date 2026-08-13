#pragma once

#include "protocol.h"

#include <deque>
#include <memory>
#include <set>
#include <string>

class ClientSession;

struct Room
{
    chatbox::protocol::RoomId id = 0;
    std::string name;
    std::string topic;
    std::string owner_nickname;
    std::deque<std::string> recent_messages;
    std::set<std::shared_ptr<ClientSession>> members;
};

