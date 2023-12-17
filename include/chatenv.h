//
// Created by LemonPig on 2023/12/16.
//

#ifndef LEMON_CHAT_CHATENV_H
#define LEMON_CHAT_CHATENV_H

#include "p2pnode.h"
#include "chatbox.h"
#include <tuple>

class ChatEnv {
private:
    int localPort;
    P2PNode node;
    ChatBox chatBox;

public:
    ChatEnv() : localPort(0){}
    ~ChatEnv() {
        node.closeConnection();
    }
    int start();
    //void service();
};

#endif //LEMON_CHAT_CHATENV_H
