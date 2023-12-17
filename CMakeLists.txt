cmake_minimum_required(VERSION 3.27)
project(LemonChat)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if (APPLE)
    set(CMAKE_PREFIX_PATH "/usr/local/opt/qt@5")
endif ()

find_package(Qt5 COMPONENTS
        Core
        Gui
        Widgets
        REQUIRED)

include_directories(include)

add_executable(LemonChat
        main.cpp
        include/chatbox.h
        include/chatenv.h
        include/p2pnode.h
        src/p2pnode.cpp
        src/chatbox.cpp
        src/chatenv.cpp
)
target_link_libraries(LemonChat
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
)

