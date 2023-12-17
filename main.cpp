#include <QApplication>
#include <QPushButton>
#include "include/chatenv.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    ChatEnv env;
    return env.start();
}
