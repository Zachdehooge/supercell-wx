cmake_minimum_required(VERSION 3.16.0)
set(PROJECT_NAME scwx-qt6ct)

#extract version from qt6ct.h
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/qt6ct/src/qt6ct-common/qt6ct.h"
     QT6CT_VERSION_DATA REGEX "^#define[ \t]+QT6CT_VERSION_[A-Z]+[ \t]+[0-9]+.*$")

if(QT6CT_VERSION_DATA)
  foreach(item IN ITEMS MAJOR MINOR)
    string(REGEX REPLACE ".*#define[ \t]+QT6CT_VERSION_${item}[ \t]+([0-9]+).*"
       "\\1" QT6CT_VERSION_${item} ${QT6CT_VERSION_DATA})
  endforeach()
  set(QT6CT_VERSION "${QT6CT_VERSION_MAJOR}.${QT6CT_VERSION_MINOR}")
  set(QT6CT_SOVERSION "${QT6CT_VERSION_MAJOR}")
  message(STATUS "qt6ct version: ${QT6CT_VERSION}")
else()
  message(FATAL_ERROR "invalid header")
endif()

add_definitions(-DQT6CT_LIBRARY)

set(app_SRCS
  qt6ct/src/qt6ct-common/qt6ct.cpp
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/qt6ct/src/qt6ct-common)

add_library(qt6ct-common STATIC ${app_SRCS})
set_target_properties(qt6ct-common PROPERTIES VERSION ${QT6CT_VERSION})
target_link_libraries(qt6ct-common PRIVATE Qt6::Gui)
install(TARGETS qt6ct-common DESTINATION ${CMAKE_INSTALL_LIBDIR})

set_target_properties(qt6ct-common PROPERTIES PUBLIC_HEADER qt6ct/src/qt6ct-common/qt6ct.h)
target_include_directories( qt6ct-common INTERFACE qt6ct/src )
