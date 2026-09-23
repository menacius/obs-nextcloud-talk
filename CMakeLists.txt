cmake_minimum_required(VERSION 3.24...3.31)

project(obs-nextcloud-talk VERSION 0.7.0 LANGUAGES C CXX)

option(NEXTCLOUD_TALK_BUILD_TESTS "Build parser and state tests" ON)

if(NOT TARGET OBS::libobs)
  find_package(libobs CONFIG REQUIRED)
endif()
if(NOT TARGET OBS::obs-frontend-api)
  find_package(obs-frontend-api CONFIG REQUIRED)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Svg Widgets)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swresample swscale)

# Nextcloud Talk uses Olm sessions to exchange the per-participant keys used by
# its encoded-frame E2EE transform. Pin the archived upstream release so native
# clients use the same interoperable protocol as the web client.
include(FetchContent)
set(OLM_TESTS OFF CACHE BOOL "" FORCE)
cmake_policy(SET CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
  olm
  GIT_REPOSITORY https://gitlab.matrix.org/matrix-org/olm.git
  GIT_TAG 7e0c8277032e40308987257b711b38af8d77cc69
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(olm)

add_library(obs-nextcloud-talk MODULE)

target_sources(
  obs-nextcloud-talk
  PRIVATE
    src/config.cpp
    src/config.hpp
    src/credential-store.cpp
    src/credential-store.hpp
    src/incoming-media-decoder.cpp
    src/incoming-media-decoder.hpp
    src/outgoing-media.cpp
    src/outgoing-media.hpp
    src/outgoing-preview.cpp
    src/outgoing-preview.hpp
    src/participant-source.cpp
    src/participant-source.hpp
    src/source-registry.cpp
    src/source-registry.hpp
    src/talk-client.cpp
    src/talk-client.hpp
    src/talk-media-transport.cpp
    src/talk-media-transport.hpp
    src/talk-e2ee.cpp
    src/talk-e2ee.hpp
    src/winhttp-websocket.cpp
    src/winhttp-websocket.hpp
    src/talk-dock.cpp
    src/talk-dock.hpp
    src/talk-types.hpp
    src/theme-icons.cpp
    src/theme-icons.hpp
    src/plugin-main.cpp
)

target_compile_features(obs-nextcloud-talk PRIVATE cxx_std_17)
target_compile_definitions(obs-nextcloud-talk PRIVATE NEXTCLOUD_TALK_VERSION="${PROJECT_VERSION}")
target_include_directories(obs-nextcloud-talk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  obs-nextcloud-talk
  PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Network
    Qt6::Svg
    Qt6::Widgets
    LibDataChannel::LibDataChannel
    Olm::Olm
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swresample
    FFmpeg::swscale
)

if(WIN32)
  target_link_libraries(obs-nextcloud-talk PRIVATE winhttp advapi32 bcrypt)
endif()

if(WIN32 AND TARGET Qt6::QSchannelBackendPlugin)
  add_custom_command(
    TARGET obs-nextcloud-talk
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:obs-nextcloud-talk>/tls"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Qt6::QSchannelBackendPlugin>"
            "$<TARGET_FILE_DIR:obs-nextcloud-talk>/tls/$<TARGET_FILE_NAME:Qt6::QSchannelBackendPlugin>"
    COMMENT "Bundling the Qt Schannel TLS backend"
  )
endif()

if(WIN32)
  add_custom_command(
    TARGET obs-nextcloud-talk
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:LibDataChannel::LibDataChannel>"
            "$<TARGET_FILE_DIR:obs-nextcloud-talk>/$<TARGET_FILE_NAME:LibDataChannel::LibDataChannel>"
    COMMENT "Bundling the WebRTC runtime"
  )
endif()

if(MSVC)
  target_compile_options(obs-nextcloud-talk PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(obs-nextcloud-talk PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(
  obs-nextcloud-talk
  PROPERTIES
    PREFIX ""
    PDB_NAME "obs-nextcloud-talk-${PROJECT_VERSION}"
)

if(COMMAND setup_plugin_target)
  setup_plugin_target(obs-nextcloud-talk)
else()
  include(GNUInstallDirs)
  if(WIN32)
    install(TARGETS obs-nextcloud-talk
      RUNTIME DESTINATION "obs-plugins/64bit"
      LIBRARY DESTINATION "obs-plugins/64bit")
    if(TARGET Qt6::QSchannelBackendPlugin)
      install(FILES "$<TARGET_FILE:Qt6::QSchannelBackendPlugin>" DESTINATION "obs-plugins/64bit/tls")
    endif()
    install(FILES "$<TARGET_FILE:LibDataChannel::LibDataChannel>" DESTINATION "obs-plugins/64bit")
    install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-nextcloud-talk")
  elseif(APPLE)
    install(TARGETS obs-nextcloud-talk LIBRARY DESTINATION "obs-plugins")
    install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-nextcloud-talk")
  else()
    install(TARGETS obs-nextcloud-talk LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    install(DIRECTORY data/ DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-nextcloud-talk")
  endif()
endif()

if(NEXTCLOUD_TALK_BUILD_TESTS)
  enable_testing()
  add_executable(nextcloud-talk-tests tests/talk-client-tests.cpp src/talk-client.cpp)
  target_compile_features(nextcloud-talk-tests PRIVATE cxx_std_17)
  target_compile_definitions(nextcloud-talk-tests PRIVATE NEXTCLOUD_TALK_TESTING)
  target_include_directories(nextcloud-talk-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(nextcloud-talk-tests PRIVATE Qt6::Core Qt6::Network)
  add_test(NAME nextcloud-talk-tests COMMAND nextcloud-talk-tests)

  add_executable(nextcloud-talk-datachannel-tests tests/data-channel-negotiation-tests.cpp)
  target_compile_features(nextcloud-talk-datachannel-tests PRIVATE cxx_std_17)
  target_link_libraries(nextcloud-talk-datachannel-tests PRIVATE LibDataChannel::LibDataChannel)
  add_test(NAME nextcloud-talk-datachannel-tests COMMAND nextcloud-talk-datachannel-tests)

  add_executable(nextcloud-talk-icon-tests tests/icon-render-tests.cpp)
  target_compile_features(nextcloud-talk-icon-tests PRIVATE cxx_std_17)
  target_compile_definitions(
    nextcloud-talk-icon-tests PRIVATE NEXTCLOUD_TALK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(nextcloud-talk-icon-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Svg)
  add_test(NAME nextcloud-talk-icon-tests COMMAND nextcloud-talk-icon-tests)
endif()
