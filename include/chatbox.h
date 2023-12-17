//
// Created by LemonPig on 2023/12/16.
//

#ifndef LEMON_CHAT_CHATBOX_H
#define LEMON_CHAT_CHATBOX_H

#include <QWidget>
#include <QTimer>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QMessageBox>
#include <QApplication>

#include <sstream>
#include <thread>
#include "p2pnode.h"

QT_BEGIN_NAMESPACE
namespace Ui { class ChatBox; }
QT_END_NAMESPACE

class ChatBox : public QWidget {
Q_OBJECT

public:
    explicit ChatBox(QWidget *parent = nullptr);

    ~ChatBox() override;
    void setParentEnv(P2PNode *parentEnv_node);
    void service();

signals:
    void newMessageReceived(const QString &message);


protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void on_newMessageReceived(const QString &message);
    void on_send_button_clicked();

private:
    Ui::ChatBox *ui;
    P2PNode* parent_p2pnode;
};


#endif //LEMON_CHAT_CHATBOX_H
