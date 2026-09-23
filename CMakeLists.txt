cmake_minimum_required(VERSION 3.24...3.31)

project(obs-nextcloud-talk VERSION 0.7.1 LANGUAGES C CXX)

option(NEXTCLOUD_TALK_BUILD_TESTS "Build parser and state tests" ON)

if(NOT TARGET OBS::libobs)
  find_package(libobs CONFIG REQUIRED)
endif()
if(NOT TARGET OBS::obs-frontend-api)
  find_package(obs-frontend-api CONFIG REQUIRED)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Svg Widgets)
if(UNIX)
  find_package(PkgConfig REQUIRED)
endif()

find_package(FFmpeg QUIET COMPONENTS avcodec avutil swresample swscale)
if(TARGET FFmpeg::avcodec AND TARGET FFmpeg::avutil AND
   TARGET FFmpeg::swresample AND TARGET FFmpeg::swscale)
  set(NEXTCLOUD_TALK_FFMPEG_TARGETS
    FFmpeg::avcodec FFmpeg::avutil FFmpeg::swresample FFmpeg::swscale)
elseif(UNIX)
  pkg_check_modules(AVCodec REQUIRED IMPORTED_TARGET libavcodec)
  pkg_check_modules(AVUtil REQUIRED IMPORTED_TARGET libavutil)
  pkg_check_modules(SWResample REQUIRED IMPORTED_TARGET libswresample)
  pkg_check_modules(SWScale REQUIRED IMPORTED_TARGET libswscale)
  set(NEXTCLOUD_TALK_FFMPEG_TARGETS
    PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::SWResample PkgConfig::SWScale)
else()
  message(FATAL_ERROR "FFmpeg development libraries were not found.")
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# OBS dependency bundles normally provide libdatachannel. Linux distributions
# often do not, so fall back to the pinned upstream release with WebSocket and
# media support enabled.
find_package(LibDataChannel 0.24.2 CONFIG QUIET)
if(NOT TARGET LibDataChannel::LibDataChannel)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
  set(NO_MEDIA OFF CACHE BOOL "" FORCE)
  set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG 251f1b407907be571a9360e6d4f8dbce55c775c0
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
  )
  FetchContent_MakeAvailable(libdatachannel)
endif()

if(NOT WIN32)
  get_target_property(_libdatachannel_definitions LibDataChannel::LibDataChannel INTERFACE_COMPILE_DEFINITIONS)
  if(_libdatachannel_definitions AND
     ";${_libdatachannel_definitions};" MATCHES ";RTC_ENABLE_WEBSOCKET=0;")
    message(FATAL_ERROR "The Linux build requires libdatachannel with WebSocket support enabled.")
  endif()
endif()

if(NOT WIN32)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()
if(UNIX AND NOT APPLE)
  pkg_check_modules(LibSecret REQUIRED IMPORTED_TARGET libsecret-1)
endif()

# Nextcloud Talk uses Olm sessions to exchange the per-participant keys used by
# its encoded-frame E2EE transform. Pin the archived upstream release so native
# clients use the same interoperable protocol as the web client.
set(OLM_TESTS OFF CACHE BOOL "" FORCE)
cmake_policy(SET CMP0077 NEW)
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
    src/winhttp-websocket.hpp
    src/talk-dock.cpp
    src/talk-dock.hpp
    src/talk-types.hpp
    src/theme-icons.cpp
    src/theme-icons.hpp
    src/plugin-main.cpp
)

if(WIN32)
  target_sources(obs-nextcloud-talk PRIVATE src/winhttp-websocket.cpp)
else()
  target_sources(obs-nextcloud-talk PRIVATE src/datachannel-websocket.cpp)
endif()

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
    ${NEXTCLOUD_TALK_FFMPEG_TARGETS}
)

if(WIN32)
  target_link_libraries(obs-nextcloud-talk PRIVATE winhttp advapi32 bcrypt)
else()
  target_link_libraries(obs-nextcloud-talk PRIVATE OpenSSL::Crypto)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(obs-nextcloud-talk PRIVATE PkgConfig::LibSecret)
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
      RUNTIME DESTINATION "obs-plugins/64bit" COMPONENT Runtime
      LIBRARY DESTINATION "obs-plugins/64bit" COMPONENT Runtime)
    if(TARGET Qt6::QSchannelBackendPlugin)
      install(FILES "$<TARGET_FILE:Qt6::QSchannelBackendPlugin>" DESTINATION "obs-plugins/64bit/tls"
        COMPONENT Runtime)
    endif()
    install(FILES "$<TARGET_FILE:LibDataChannel::LibDataChannel>" DESTINATION "obs-plugins/64bit"
      COMPONENT Runtime)
    install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-nextcloud-talk" COMPONENT Runtime)
  elseif(APPLE)
    install(TARGETS obs-nextcloud-talk LIBRARY DESTINATION "obs-plugins" COMPONENT Runtime)
    install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-nextcloud-talk" COMPONENT Runtime)
  else()
    install(TARGETS obs-nextcloud-talk LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
      COMPONENT Runtime)
    install(DIRECTORY data/ DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-nextcloud-talk"
      COMPONENT Runtime)
  endif()
  install(FILES LICENSE README.md DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/obs-nextcloud-talk"
    COMPONENT Runtime)
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

  add_executable(nextcloud-talk-e2ee-tests tests/e2ee-roundtrip-tests.cpp src/talk-e2ee.cpp)
  target_compile_features(nextcloud-talk-e2ee-tests PRIVATE cxx_std_17)
  target_compile_definitions(nextcloud-talk-e2ee-tests PRIVATE NEXTCLOUD_TALK_TESTING)
  target_include_directories(nextcloud-talk-e2ee-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(nextcloud-talk-e2ee-tests PRIVATE Qt6::Core Olm::Olm)
  if(WIN32)
    target_link_libraries(nextcloud-talk-e2ee-tests PRIVATE bcrypt)
  else()
    target_link_libraries(nextcloud-talk-e2ee-tests PRIVATE OpenSSL::Crypto)
  endif()
  add_test(NAME nextcloud-talk-e2ee-tests COMMAND nextcloud-talk-e2ee-tests)
endif()
