//
// Created by LemonPig on 2023/12/16.
//

#include "../include/chatenv.h"

int ChatEnv::start() {
    chatBox.setParentEnv(&(this->node));
    chatBox.show();

    QTimer::singleShot(0, [this](){
        emit chatBox.newMessageReceived(QString::fromUtf8("Available IPv4 addresses:\n"));
        std::vector<std::string> localIPs = getLocalIPs();
        for (const std::string &ip : localIPs) {
            std::string message = " - " + ip + "\n";
            emit chatBox.newMessageReceived(QString::fromStdString(message));
        }
        emit chatBox.newMessageReceived(QString::fromUtf8("你还没有绑定端口，请输入在聊天框输入端口号并点击发送以继续\n"));
    });

    QApplication::exec();

    return 0;
}