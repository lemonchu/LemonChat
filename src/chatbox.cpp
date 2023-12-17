//
// Created by LemonPig on 2023/12/16.
//

// You may need to build the project (run Qt uic code generator) to get "ui_ChatBox.h" resolved

#include "../include/chatbox.h"
#include "ui_ChatBox.h"


ChatBox::ChatBox(QWidget *parent) : QWidget(parent), ui(new Ui::ChatBox) {
    ui->setupUi(this);
    connect(this, &ChatBox::newMessageReceived, this, &ChatBox::on_newMessageReceived);
}

ChatBox::~ChatBox() {
    delete ui;
}

void ChatBox::on_newMessageReceived(const QString &message) {
    ui->msg_board->insertPlainText(message);
}

void ChatBox::on_send_button_clicked() {
    // 将光标移动到文本末尾
    QTextCursor cursor = ui->msg_board->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->msg_board->setTextCursor(cursor);

    if(!this->parent_p2pnode->isInited()) {
        bool is_port = false;
        int port = ui->msg_box->toPlainText().toInt(&is_port, 10);

        if(!is_port) {
            ui->msg_board->insertPlainText(QString::fromUtf8("端口号不合法，请重新输入\n"));
            return;
        }
        if(port < 1024 || port > 65535) {
            ui->msg_board->insertPlainText(QString::fromUtf8("端口号不合法，请重新输入: 端口号应该在1024-65535之间\n"));
            return;
        }

        this->parent_p2pnode->initialize(port);
        ui->msg_board->insertPlainText(QString::fromUtf8("端口绑定成功！端口号："));
        ui->msg_board->insertPlainText(QString::number(port));
        ui->msg_board->insertPlainText(QString::fromUtf8("\n"));
        ui->msg_box->clear();
        std::thread listenThread(&ChatBox::service, this);
        listenThread.detach();
        return;
    }

    QString msg = ui->msg_box->toPlainText();
    QString ip = ui->ip->text();
    QString port = ui->port->text();

    this->parent_p2pnode->send(msg.toStdString(), ip.toStdString(), port.toInt());


    std::ostringstream oss;
    oss << "------  You: to " << ip.toStdString() << ":" << port.toInt() << "\n\n";

    ui->msg_board->insertPlainText(QString::fromStdString(oss.str()));
    ui->msg_board->insertPlainText(msg);
    ui->msg_board->insertPlainText(QString::fromUtf8("\n\n"));
    ui->msg_box->clear();
}

void ChatBox::closeEvent(QCloseEvent *event) {
    // Here you can define what happens when the window is about to close.
    // For example, you can ask the user for confirmation before closing the window.

    QMessageBox::StandardButton resBtn = QMessageBox::question( this, "Application name",
                                                                tr("Are you sure?\n"),
                                                                QMessageBox::Cancel | QMessageBox::No | QMessageBox::Yes,
                                                                QMessageBox::Yes);
    if (resBtn != QMessageBox::Yes) {
        event->ignore();
    } else {
        event->accept();
    }
}

void ChatBox::setParentEnv(P2PNode *parentEnv_node) {
    parent_p2pnode = parentEnv_node;
}

void ChatBox::service() {
    while (true) {
        if(!this->parent_p2pnode->isInited()) {
            // Thread Exit
            break;
        }
        auto message = this->parent_p2pnode->receive();
        if (std::get<2>(message) == 0) {
            // Thread Exit
            break;
        }
        // Process the received message here
        std::string sender_ip = std::get<1>(message);
        int sender_port = std::get<2>(message);
        std::string buffer = std::get<0>(message);

        std::ostringstream oss;
        oss << "------  Message from " << sender_ip << ":" << sender_port << "\n\n" << buffer << "\n\n";
        std::string b_message = oss.str();
        // Print it to the board
        emit this->newMessageReceived(QString::fromStdString(b_message));
    }
}