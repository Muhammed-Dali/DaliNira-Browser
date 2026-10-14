cmake_minimum_required(VERSION 3.20)
project(DaliNiraBrowser VERSION 8.0.2 LANGUAGES CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Default install prefix" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(DALINIRA_ENABLE_ASAN "Enable AddressSanitizer for developer builds" OFF)
option(DALINIRA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for developer builds" OFF)
if(DALINIRA_ENABLE_ASAN OR DALINIRA_ENABLE_UBSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "DaliNira sanitizer options currently support GCC and Clang only")
  endif()
  set(DALINIRA_SANITIZER_FLAGS)
  if(DALINIRA_ENABLE_ASAN)
    list(APPEND DALINIRA_SANITIZER_FLAGS -fsanitize=address)
  endif()
  if(DALINIRA_ENABLE_UBSAN)
    list(APPEND DALINIRA_SANITIZER_FLAGS -fsanitize=undefined)
  endif()
  add_compile_options(${DALINIRA_SANITIZER_FLAGS} -fno-omit-frame-pointer)
  add_link_options(${DALINIRA_SANITIZER_FLAGS})
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS
  Core
  Gui
  Widgets
  WebEngineWidgets
  WebChannel
  Network
  Svg
  Concurrent
  DBus
)
find_package(SQLite3 REQUIRED)
# FindSQLite3 names its target SQLite3::SQLite3 since CMake 4.1 and
# SQLite::SQLite3 before (Debian 12: 3.25, Ubuntu 24.04: 3.28).
if(NOT TARGET SQLite3::SQLite3 AND TARGET SQLite::SQLite3)
  add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PSL REQUIRED IMPORTED_TARGET libpsl)
link_libraries(PkgConfig::PSL)
pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
if(LIBSECRET_FOUND)
  add_compile_definitions(DALINIRA_HAS_LIBSECRET=1)
  link_libraries(PkgConfig::LIBSECRET)
endif()
find_program(NODE_EXECUTABLE node REQUIRED)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/core
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/desktop_tabs
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/newtab
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/audio
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/pulse
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/blocker
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/passwords
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/eq
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/settings
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/session
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/downloads
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/translate
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/i18n
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/engine
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/onboarding
)

# -------------------------------------------------------------
# DALI Lang Web Audio DSP & Policy generation
# -------------------------------------------------------------
set(DALI_WEB_OUTPUT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/web-output-audiophile.dl)
set(DALI_WEB_OUTPUT_MODULE ${CMAKE_CURRENT_BINARY_DIR}/dali/web-output-audiophile.generated.js)
set(DALI_WEB_EQ32_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/web-equalizer-32.dl)
set(DALI_WEB_EQ32_MODULE ${CMAKE_CURRENT_BINARY_DIR}/dali/web-equalizer-32.generated.js)
set(DALI_WEB_COMPRESSOR_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/web-dynamic-compressor.dl)
set(DALI_WEB_COMPRESSOR_MODULE ${CMAKE_CURRENT_BINARY_DIR}/dali/web-dynamic-compressor.generated.js)
set(DALI_WEB_LIMITER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/web-limiter.dl)
set(DALI_WEB_LIMITER_MODULE ${CMAKE_CURRENT_BINARY_DIR}/dali/web-limiter.generated.js)
set(DALI_WEB_BASS_ENHANCER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/web-bass-enhancer.dl)
set(DALI_WEB_BASS_ENHANCER_MODULE ${CMAKE_CURRENT_BINARY_DIR}/dali/web-bass-enhancer.generated.js)
set(DALI_WEB_AUTO_GAIN_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/web-auto-gain.dl)
set(DALI_WEB_AUTO_GAIN_MODULE ${CMAKE_CURRENT_BINARY_DIR}/dali/web-auto-gain.generated.js)

add_custom_command(
  OUTPUT ${DALI_WEB_OUTPUT_MODULE} ${DALI_WEB_EQ32_MODULE} ${DALI_WEB_COMPRESSOR_MODULE} ${DALI_WEB_LIMITER_MODULE} ${DALI_WEB_BASS_ENHANCER_MODULE} ${DALI_WEB_AUTO_GAIN_MODULE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/dali
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js ${DALI_WEB_OUTPUT_SOURCE} ${DALI_WEB_OUTPUT_MODULE}
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js ${DALI_WEB_EQ32_SOURCE} ${DALI_WEB_EQ32_MODULE}
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js ${DALI_WEB_COMPRESSOR_SOURCE} ${DALI_WEB_COMPRESSOR_MODULE}
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js ${DALI_WEB_LIMITER_SOURCE} ${DALI_WEB_LIMITER_MODULE}
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js ${DALI_WEB_BASS_ENHANCER_SOURCE} ${DALI_WEB_BASS_ENHANCER_MODULE}
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js ${DALI_WEB_AUTO_GAIN_SOURCE} ${DALI_WEB_AUTO_GAIN_MODULE}
  DEPENDS
    ${DALI_WEB_OUTPUT_SOURCE}
    ${DALI_WEB_EQ32_SOURCE}
    ${DALI_WEB_COMPRESSOR_SOURCE}
    ${DALI_WEB_LIMITER_SOURCE}
    ${DALI_WEB_BASS_ENHANCER_SOURCE}
    ${DALI_WEB_AUTO_GAIN_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/compile-dali-preset.js
    ${CMAKE_CURRENT_SOURCE_DIR}/dali-lang/src/index.js
    ${CMAKE_CURRENT_SOURCE_DIR}/dali-lang/src/compiler-web-audio.js
  VERBATIM
)
add_custom_target(dalinira-web-output-dali DEPENDS ${DALI_WEB_OUTPUT_MODULE} ${DALI_WEB_EQ32_MODULE} ${DALI_WEB_COMPRESSOR_MODULE} ${DALI_WEB_LIMITER_MODULE} ${DALI_WEB_BASS_ENHANCER_MODULE} ${DALI_WEB_AUTO_GAIN_MODULE})

set(DALI_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/browser/dali/browser.dali)
set(DALI_POLICY_GENERATOR ${CMAKE_CURRENT_SOURCE_DIR}/browser/scripts/generate-browser-policy.js)
set(DALI_POLICY_JSON ${CMAKE_CURRENT_BINARY_DIR}/browser_policy.json)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/assets/new-tab)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/assets/new-tab/icons)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/adblock
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/resources)
# configure_file (unlike file(COPY)) re-copies a catalog whenever it changes,
# so tests that read build/i18n never see stale translations.
foreach(I18N_FILE languages.json translations/tr.json translations/en.json
                  translations/ar.json translations/fr.json)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/i18n/${I18N_FILE}
                 ${CMAKE_CURRENT_BINARY_DIR}/i18n/${I18N_FILE} COPYONLY)
endforeach()

foreach(NTP_ASSET dalinira-flow-blue.png google.ico duckduckgo.ico startpage.ico mojeek.ico)
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/new-tab/${NTP_ASSET}
    ${CMAKE_CURRENT_BINARY_DIR}/assets/new-tab/${NTP_ASSET}
    COPYONLY
  )
endforeach()
foreach(NTP_ICON appearance.svg settings.svg search.svg grid.svg clock.svg cards.svg close.svg)
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/browser-icons/${NTP_ICON}
    ${CMAKE_CURRENT_BINARY_DIR}/assets/new-tab/icons/${NTP_ICON}
    COPYONLY
  )
endforeach()
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-128.png
  ${CMAKE_CURRENT_BINARY_DIR}/assets/new-tab/dalinira-browser.png
  COPYONLY
)

add_custom_command(
  OUTPUT ${DALI_POLICY_JSON}
  COMMAND ${NODE_EXECUTABLE} ${DALI_POLICY_GENERATOR} ${DALI_MANIFEST} ${DALI_POLICY_JSON}
  DEPENDS ${DALI_MANIFEST} ${DALI_POLICY_GENERATOR} ${CMAKE_CURRENT_SOURCE_DIR}/dali-lang/src/index.js
  VERBATIM
)
add_custom_target(dalinira-browser-policy DEPENDS ${DALI_POLICY_JSON})

# -------------------------------------------------------------
# Source list
# -------------------------------------------------------------
set(CORE_SOURCES
  browser/native/core/application_identity.cpp
  browser/native/core/default_browser_integration.cpp
  browser/native/core/browser_profile_service.cpp
  browser/native/core/browser_permission_policy.cpp
  browser/native/core/site_permission_prompt_bubble.cpp
  browser/native/core/site_controls_bubble.cpp
  browser/native/core/bookmark_bar_reorder.cpp
  browser/native/core/performance_diagnostics.cpp
  browser/native/core/browser_policy.cpp
  browser/native/core/security_utils.cpp
  browser/native/core/web_engine_hardware_acceleration.cpp
  browser/native/core/web_engine_memory_policy.cpp
  browser/native/core/browser_icons.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/search_suggestion_service.cpp
  browser/native/core/domain_normalizer.cpp
  browser/native/core/history_candidate_provider.cpp
  browser/native/core/bookmark_candidate_provider.cpp
  browser/native/core/frequent_sites_candidate_provider.cpp
   browser/native/core/composite_navigation_candidate_provider.cpp
  browser/native/core/address_input_resolver.cpp
  browser/native/core/adult_content_protection.cpp
  browser/native/core/keyboard_shortcut_service.cpp
)

set(NEWTAB_SOURCES
  browser/native/newtab/new_tab_asset_resolver.cpp
  browser/native/newtab/new_tab_html.cpp
  browser/native/newtab/new_tab_scheme.cpp
  browser/native/newtab/new_tab_background_store.cpp
)

set(DESKTOP_TABS_SOURCES
  browser/native/desktop_tabs/tab_manager.cpp
  browser/native/desktop_tabs/tab_performance_manager.cpp
  browser/native/desktop_tabs/tab_lifecycle_adapter.cpp
  browser/native/desktop_tabs/tab_resource_model.cpp
  browser/native/desktop_tabs/system_memory_pressure_monitor.cpp
  browser/native/desktop_tabs/tab_throbber.cpp
  browser/native/desktop_tabs/tab_hover_card.cpp
  browser/native/desktop_tabs/tab_appearance.cpp
  browser/native/desktop_tabs/tab_layout_model.cpp
  browser/native/desktop_tabs/tab_strip_animator.cpp
  browser/native/desktop_tabs/tab_strip_widget.cpp
  browser/native/desktop_tabs/tab_search_popup.cpp
  browser/native/desktop_tabs/find_bar_widget.cpp
  browser/native/desktop_tabs/tab_group_model.cpp
  browser/native/desktop_tabs/tab_group_popup.cpp
  browser/native/desktop_tabs/tab_group_launcher_popup.cpp
  browser/native/desktop_tabs/tab_drag_session.cpp
  browser/native/desktop_tabs/tab_window_registry.cpp
  browser/native/desktop_tabs/detached_tab_window_controller.cpp
  browser/native/desktop_tabs/tab_drag_controller.cpp
  browser/native/desktop_tabs/platform/tab_drag_platform.cpp
)

set(SESSION_SOURCES
  browser/native/session/session_store.cpp
)

set(SETTINGS_SOURCES
  browser/native/settings/feedback_links.cpp
  browser/native/settings/settings_page.cpp
  browser/native/settings/settings_content_section.cpp
  browser/native/settings/settings_performance_section.cpp
  browser/native/settings/settings_privacy_section.cpp
  browser/native/settings/settings_languages_section.cpp
  browser/native/settings/settings_shortcuts_section.cpp
)

set(ONBOARDING_SOURCES
  browser/native/onboarding/browser_data_importer.cpp
  browser/native/onboarding/engine_step.cpp
  browser/native/onboarding/import_step.cpp
  browser/native/onboarding/layout_step.cpp
  browser/native/onboarding/search_privacy_step.cpp
  browser/native/onboarding/ready_step.cpp
  browser/native/onboarding/onboarding_manager.cpp
  browser/native/onboarding/onboarding_step.cpp
  browser/native/onboarding/onboarding_view.cpp
  browser/native/onboarding/welcome_step.cpp
  browser/native/onboarding/aurora_wave_background.cpp
  browser/native/onboarding/onboarding_window.cpp
)

set(AUDIO_SOURCES
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/web_audio_effects_controller.cpp
  browser/native/audio/audio_effects_page.cpp
  browser/native/audio/audio_device_manager.cpp
  browser/native/audio/audio_capture_service.cpp
)

set(EQ_SOURCES
  browser/native/eq/eq_preset_repository.cpp
  browser/native/eq/eq_preset_page.cpp
)

set(BLOCKER_SOURCES
  browser/native/blocker/dalinira_blocker_settings.cpp
  browser/native/blocker/dalinira_blocker_engine.cpp
  browser/native/blocker/dalinira_blocker_list_manager.cpp
  browser/native/blocker/dalinira_blocker_filter_updates.cpp
  browser/native/blocker/dalinira_blocker_redirect_resources.cpp
  browser/native/blocker/dalinira_blocker_element_picker.cpp
  browser/native/blocker/dalinira_blocker_request_interceptor.cpp
  browser/native/blocker/dalinira_blocker_service.cpp
  browser/native/blocker/dalinira_blocker_page.cpp
  browser/native/blocker/dalinira_blocker_shield_button.cpp
)

set(PASSWORDS_SOURCES
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
  browser/native/passwords/credential_autofill_controller.cpp
  browser/native/passwords/webkit_credential_controller.cpp
  browser/native/passwords/credential_save_bubble.cpp
  browser/native/passwords/vault_unlock_dialog.cpp
  browser/native/passwords/password_manager_page.cpp
)

set(PULSE_SOURCES
  browser/native/pulse/song_finder_settings.cpp
  browser/native/pulse/song_fingerprint.cpp
  browser/native/pulse/song_recognition_service.cpp
  browser/native/pulse/big_listen_button.cpp
  browser/native/pulse/song_finder_page.cpp
  browser/native/pulse/song_finder_settings_page.cpp
  browser/native/pulse/pulse_toolbar_button.cpp
)

set(DOWNLOADS_SOURCES
  browser/native/downloads/media_download_service.cpp
  browser/native/downloads/media_download_page.cpp
  browser/native/downloads/local_media_routing.cpp
  browser/native/downloads/local_media_player_page.cpp
  browser/native/downloads/general_download_manager.cpp
  browser/native/downloads/download_ui_model.cpp
  browser/native/downloads/download_toolbar_ui.cpp
  browser/native/downloads/yt_dlp_update_manager.cpp
  browser/native/downloads/media_platform_registry.cpp
  browser/native/downloads/media_page_url_resolver.cpp
  browser/native/downloads/download_desktop_actions.cpp
)

set(TRANSLATE_SOURCES
  browser/native/translate/translate_service.cpp
  browser/native/translate/translation_secret_store.cpp
  browser/native/translate/providers/libretranslate_provider.cpp
  browser/native/translate/providers/deepl_provider.cpp
  browser/native/translate/providers/google_cloud_provider.cpp
  browser/native/translate/providers/google_gtx_provider.cpp
  browser/native/translate/language_detector.cpp
  browser/native/translate/page_translator.cpp
  browser/native/translate/translate_bubble_popup.cpp
)

set(I18N_SOURCES
  browser/native/i18n/language_manager.cpp
)

set(WINDOW_SOURCES
  browser/native/browser_window.cpp
  browser/native/browser_window_menus.cpp
  browser/native/browser_window_page_actions.cpp
  browser/native/browser_window_shortcuts.cpp
  browser/native/layout/browser_layout.cpp
  browser/native/layout/browser_layout_controller.cpp
  browser/native/layout/browser_side_panel.cpp
)

set(ENGINE_SOURCES
  browser/native/engine/engine_backend.cpp
  browser/native/engine/engine_view.cpp
  browser/native/engine/engine_profile.cpp
  browser/native/engine/engine_storage_manager.cpp
  browser/native/engine/display_platform.cpp
  browser/native/engine/chromium/chromium_backend.cpp
  browser/native/engine/chromium/chromium_engine_view.cpp
)

option(DALINIRA_ENABLE_WEBKIT "Enable WebKit backend (WebCore/JavaScriptCore)" ON)
option(DALINIRA_REQUIRE_DUAL_ENGINE
  "Fail configuration unless both Chromium and WebKit backends are available" ON)
set(DALINIRA_BUILD_WEBKIT 0)
# DaliNira's private, patched WebKitGTK runtime (packaging/webkit-runtime):
# release packages build against the SDK that build-webkit-runtime.sh makes
# and install the runtime under lib/dalinira-browser/webkit.
set(DALINIRA_WEBKIT_RUNTIME_SDK "" CACHE PATH
    "Package SDK made by packaging/webkit-runtime/build-webkit-runtime.sh")
# The normal application build can use the preserved local validation build
# directly. This is a developer-only input; release packages always pass the
# SDK above and never retain this workstation path.
set(DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME
    "${CMAKE_SOURCE_DIR}/build-webkit-runtime/prefix" CACHE PATH
    "Private patched WebKitGTK development prefix made by scripts/build-dalinira.sh")
# TEST-ONLY escape hatch for CI jobs that cannot afford a WebKitGTK build:
# compile the WebKit backend against the system WebKitGTK. Such a build
# cannot be installed or packaged, and it cannot be combined with a package
# SDK. Normal and release builds leave it OFF and require the private runtime.
option(DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS
  "Test-only: build the WebKit backend against the unpatched system WebKitGTK (never for releases)" OFF)
if(DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS AND DALINIRA_WEBKIT_RUNTIME_SDK)
  message(FATAL_ERROR
    "DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS is test-only and cannot be combined "
    "with DALINIRA_WEBKIT_RUNTIME_SDK (a package build).")
endif()
set(DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME_ACTIVE OFF)
if(DALINIRA_ENABLE_WEBKIT AND DALINIRA_WEBKIT_RUNTIME_SDK)
  include("${DALINIRA_WEBKIT_RUNTIME_SDK}/dalinira-webkit-runtime.cmake")
  # WebKitGTK compiles in the paths of its helper processes, so the runtime
  # must be installed exactly at the prefix it was built for.
  if(NOT DALINIRA_WEBKIT_RUNTIME_PREFIX STREQUAL "${CMAKE_INSTALL_PREFIX}/lib/dalinira-browser/webkit")
    message(FATAL_ERROR "The WebKit runtime SDK was built for ${DALINIRA_WEBKIT_RUNTIME_PREFIX}, "
                        "but this build installs to ${CMAKE_INSTALL_PREFIX}/lib/dalinira-browser/webkit")
  endif()
  set(ENV{PKG_CONFIG_PATH} "${DALINIRA_WEBKIT_RUNTIME_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
  add_compile_definitions(DALINIRA_WEBKIT_PRIVATE_RUNTIME_PREFIX="${DALINIRA_WEBKIT_RUNTIME_PREFIX}")
  # DT_RPATH, not DT_RUNPATH: searched before LD_LIBRARY_PATH, so the private
  # libraries win over any system WebKitGTK whatever the environment says.
  add_link_options(LINKER:--disable-new-dtags)
  # Build tree: the install location first (a staged package mounted there for
  # the GUI tests), then the SDK itself.
  set(CMAKE_BUILD_RPATH "${DALINIRA_WEBKIT_RUNTIME_PREFIX}/lib;${DALINIRA_WEBKIT_RUNTIME_STAGE}/lib")
  message(STATUS "WebKit: private runtime WebKitGTK ${DALINIRA_WEBKIT_RUNTIME_VERSION} "
                 "(revision ${DALINIRA_WEBKIT_RUNTIME_REVISION}) at ${DALINIRA_WEBKIT_RUNTIME_PREFIX}")
elseif(DALINIRA_ENABLE_WEBKIT AND DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME AND
       EXISTS "${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}/lib/pkgconfig/webkit2gtk-4.1.pc")
  get_filename_component(DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME
    "${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}" REALPATH)
  set(DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME_ACTIVE ON)
  set(ENV{PKG_CONFIG_PATH}
      "${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  add_compile_definitions(
    DALINIRA_WEBKIT_PRIVATE_RUNTIME_PREFIX="${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}"
    DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME=1)
  set(CMAKE_BUILD_RPATH "${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}/lib")
  message(STATUS "WebKit: preserved private development runtime at "
                 "${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}")
endif()
set(DALINIRA_SYSTEM_WEBKIT_TEST_BUILD OFF)
if(DALINIRA_ENABLE_WEBKIT AND NOT DALINIRA_WEBKIT_RUNTIME_SDK AND
   NOT DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME_ACTIVE)
  if(NOT DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS)
    message(FATAL_ERROR
      "DaliNira's WebKit engine is built only against its private patched "
      "WebKitGTK runtime, and none was found at "
      "${DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME}. The unpatched system WebKitGTK is "
      "not used as a substitute.\n"
      "Run ./scripts/build-dalinira.sh: on a fresh checkout it builds the "
      "runtime once (or run ./scripts/bootstrap-webkit-runtime.sh first). "
      "Package builds pass -DDALINIRA_WEBKIT_RUNTIME_SDK=DIR. See docs/BUILDING.md.")
  endif()
  set(DALINIRA_SYSTEM_WEBKIT_TEST_BUILD ON)
  add_compile_definitions(DALINIRA_SYSTEM_WEBKIT_TEST_BUILD=1)
  message(WARNING
    "TEST-ONLY build: the WebKit backend uses the unpatched system WebKitGTK "
    "(DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS=ON). It cannot be installed or packaged.")
endif()
if(DALINIRA_ENABLE_WEBKIT)
  # gstreamer-1.0: the UI process checks the same registry WebKit's media stack uses.
  pkg_check_modules(WEBKIT webkit2gtk-4.1 gstreamer-1.0)
  if(DALINIRA_WEBKIT_RUNTIME_SDK OR DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME_ACTIVE)
    # Full paths, so the link uses the SDK's libraries and not a system copy.
    set(WEBKIT_LIBRARIES ${WEBKIT_LINK_LIBRARIES})
  endif()
  pkg_check_modules(X11 x11 xi xcb)
  if(WEBKIT_FOUND AND X11_FOUND)
    set(DALINIRA_BUILD_WEBKIT 1)
    add_compile_definitions(DALINIRA_BUILD_WEBKIT=1)
    message(STATUS "WebKit backend enabled (WebKitGTK ${WEBKIT_VERSION})")
    include_directories(${WEBKIT_INCLUDE_DIRS} ${X11_INCLUDE_DIRS})
    link_libraries(${WEBKIT_LIBRARIES} ${X11_LIBRARIES})
    list(APPEND ENGINE_SOURCES
      browser/native/engine/webkit/webkit_backend.cpp
      browser/native/engine/webkit/webkit_engine_view.cpp
      browser/native/engine/webkit/webkit_engine_profile.cpp
      browser/native/engine/webkit/webkit_media_runtime.cpp
      browser/native/engine/webkit/webkit_network_filter.cpp
      browser/native/engine/webkit/webkit_private_runtime.cpp
    )
    # Loaded by WebKitGTK into DaliNira's web processes (network and audio policy).
    # It lives next to the executable, in the build tree and when installed.
    pkg_check_modules(WEBKIT_EXTENSION REQUIRED webkit2gtk-web-extension-4.1 gstreamer-1.0)
    if(DALINIRA_WEBKIT_RUNTIME_SDK OR DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME_ACTIVE)
      set(WEBKIT_EXTENSION_LIBRARIES ${WEBKIT_EXTENSION_LINK_LIBRARIES})
    endif()
    add_library(dalinira-webkit-extension MODULE
      browser/native/engine/webkit/extension/dalinira_web_extension.cpp)
    target_include_directories(dalinira-webkit-extension PRIVATE ${WEBKIT_EXTENSION_INCLUDE_DIRS})
    # Only the web-extension API: not the directory-wide UI-process libraries.
    set_property(TARGET dalinira-webkit-extension PROPERTY LINK_LIBRARIES ${WEBKIT_EXTENSION_LIBRARIES})
    set_target_properties(dalinira-webkit-extension PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/webkit-extensions
      CXX_VISIBILITY_PRESET hidden
      AUTOMOC OFF)
  else()
    message(WARNING "WebKit requested but webkit2gtk-4.1 or x11 not found. WebKit backend disabled.")
  endif()
endif()
if(DALINIRA_REQUIRE_DUAL_ENGINE AND NOT DALINIRA_BUILD_WEBKIT)
  message(FATAL_ERROR
    "A normal DaliNira build requires both Chromium and WebKit. "
    "Use scripts/build-dalinira.sh with build-webkit-runtime/prefix. "
    "For an intentional Chromium-only diagnostic build, explicitly set both "
    "-DDALINIRA_REQUIRE_DUAL_ENGINE=OFF and -DDALINIRA_ENABLE_WEBKIT=OFF.")
endif()
set(DALINIRA_BUILD_WEBKIT ${DALINIRA_BUILD_WEBKIT} CACHE INTERNAL
  "Whether the WebKit backend is compiled into DaliNira" FORCE)

# Gecko is known as an engine identity ("gecko" workspace, --engine gecko),
# but it is not a backend of the browser yet: DALINIRA_BUILD_GECKO is never
# defined, so every Gecko request starts Chromium. DALINIRA_ENABLE_GECKO
# builds only the separate Gecko adapter library (the out-of-process Gecko
# host's launcher, authenticated IPC, session and X11 embedder) and its
# tests; dalinira-browser neither compiles nor links any of it.
option(DALINIRA_ENABLE_GECKO "Build the optional Gecko adapter library and its tests (not used by the browser yet)" OFF)
set(DALINIRA_GECKO_DEVELOPMENT_RUNTIME "" CACHE PATH
    "Development Gecko runtime (a Nightly objdir's dist/bin) for the Gecko host tests")
if(DALINIRA_ENABLE_GECKO)
  pkg_check_modules(DALINIRA_GECKO_XCB REQUIRED IMPORTED_TARGET xcb xcb-res)
  set(DALINIRA_GECKO_HOST_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/gecko-host")
  add_library(dalinira-gecko-adapter STATIC
    browser/native/engine/gecko/gecko_runtime.cpp
    browser/native/engine/gecko/gecko_runtime.h
    browser/native/engine/gecko/gecko_host_process.cpp
    browser/native/engine/gecko/gecko_host_process.h
    browser/native/engine/gecko/gecko_ipc_server.cpp
    browser/native/engine/gecko/gecko_ipc_server.h
    browser/native/engine/gecko/gecko_session.cpp
    browser/native/engine/gecko/gecko_session.h
    browser/native/engine/gecko/gecko_x11_embedder.cpp
    browser/native/engine/gecko/gecko_x11_embedder.h)
  # Only what the adapter uses: not the directory-wide WebKit libraries.
  set_property(TARGET dalinira-gecko-adapter PROPERTY LINK_LIBRARIES "")
  target_link_libraries(dalinira-gecko-adapter PUBLIC Qt6::Core Qt6::Network Qt6::Widgets PkgConfig::DALINIRA_GECKO_XCB)
  target_compile_definitions(dalinira-gecko-adapter PRIVATE
    DALINIRA_GECKO_HOST_APP_DIR="${DALINIRA_GECKO_HOST_APP_DIR}")
  if(DALINIRA_GECKO_DEVELOPMENT_RUNTIME)
    target_compile_definitions(dalinira-gecko-adapter PRIVATE
      DALINIRA_GECKO_DEVELOPMENT_RUNTIME="${DALINIRA_GECKO_DEVELOPMENT_RUNTIME}")
  endif()
  target_compile_options(dalinira-gecko-adapter PRIVATE -Wall -Wextra)

  # Speaks the host side of the protocol for the adapter tests; started from
  # a test runtime exactly as Gecko's `firefox` is.
  add_executable(dalinira-gecko-fake-host browser/native/tests/gecko_fake_host.cpp)
  set_property(TARGET dalinira-gecko-fake-host PROPERTY LINK_LIBRARIES "")
  target_link_libraries(dalinira-gecko-fake-host PRIVATE Qt6::Core Qt6::Network)

  add_executable(dalinira-browser-gecko-adapter-test browser/native/tests/gecko_adapter_test.cpp)
  set_property(TARGET dalinira-browser-gecko-adapter-test PROPERTY LINK_LIBRARIES "")
  target_link_libraries(dalinira-browser-gecko-adapter-test PRIVATE dalinira-gecko-adapter)
  target_compile_definitions(dalinira-browser-gecko-adapter-test PRIVATE
    DALINIRA_GECKO_HOST_APP_DIR="${DALINIRA_GECKO_HOST_APP_DIR}"
    DALINIRA_GECKO_FAKE_HOST="$<TARGET_FILE:dalinira-gecko-fake-host>")
  add_dependencies(dalinira-browser-gecko-adapter-test dalinira-gecko-fake-host)
  add_test(NAME browser-gecko-adapter COMMAND dalinira-browser-gecko-adapter-test)
  set_tests_properties(browser-gecko-adapter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

  # The real Gecko host, headless (no display, no GPU), on
  # DALINIRA_GECKO_DEVELOPMENT_RUNTIME. Opt-in: DALINIRA_GECKO_HOST_TESTS=1.
  add_executable(dalinira-browser-gecko-host-test browser/native/tests/gecko_host_test.cpp)
  set_property(TARGET dalinira-browser-gecko-host-test PROPERTY LINK_LIBRARIES "")
  target_link_libraries(dalinira-browser-gecko-host-test PRIVATE dalinira-gecko-adapter)
  add_test(NAME browser-gecko-host COMMAND dalinira-browser-gecko-host-test)
  set_tests_properties(browser-gecko-host PROPERTIES
    LABELS "gecko" SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

set(BROWSER_SOURCES
  ${CORE_SOURCES}
  ${NEWTAB_SOURCES}
  ${DESKTOP_TABS_SOURCES}
  ${SESSION_SOURCES}
  ${SETTINGS_SOURCES}
  ${AUDIO_SOURCES}
  ${EQ_SOURCES}
  ${BLOCKER_SOURCES}
  ${PASSWORDS_SOURCES}
  ${PULSE_SOURCES}
  ${DOWNLOADS_SOURCES}
  ${TRANSLATE_SOURCES}
  ${I18N_SOURCES}
  ${WINDOW_SOURCES}
  ${ENGINE_SOURCES}
  ${ONBOARDING_SOURCES}
  browser/native/main.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(dalinira-webengine-process
    browser/native/core/web_engine_process_launcher.cpp
  )
  # A plain POSIX launcher: none of the directory-wide link_libraries (WebKitGTK,
  # X11, libpsl, libsecret). Without --as-needed (Fedora) they became NEEDED
  # entries of every Chromium helper process, unresolvable without an RPATH.
  set_property(TARGET dalinira-webengine-process PROPERTY LINK_LIBRARIES "")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dalinira-webengine-process PRIVATE -Wall -Wextra)
  endif()
endif()

set(DALINIRA_ADULT_DOMAINS_MANAGED_SOURCE
  "${CMAKE_CURRENT_BINARY_DIR}/upstream_porn_blocklist_production.txt")
set(DALINIRA_ADULT_DOMAINS_SOURCE
  "${DALINIRA_ADULT_DOMAINS_MANAGED_SOURCE}"
  CACHE FILEPATH "Production domain text used to build adult_domains.bin")
set(DALINIRA_ADULT_DOMAINS_SNAPSHOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/privacy/adult_domains.bin")
set(DALINIRA_ADULT_DOMAINS_UPSTREAM_COMMIT
  "ee3bdbaddf49c756cf3340bb2e6a9dee5282d1c6")
set(DALINIRA_ADULT_DOMAINS_UPSTREAM_URL
  "https://raw.githubusercontent.com/blocklistproject/Lists/${DALINIRA_ADULT_DOMAINS_UPSTREAM_COMMIT}/alt-version/porn-nl.txt")
set(DALINIRA_ADULT_DOMAINS_UPSTREAM_SHA256
  "5e876678921406025032ac6fa9908cfce98216f6af0f7c0cd8e74a334b232dd4")
set(DALINIRA_ADULT_DOMAINS_FETCH_SCRIPT
  "${CMAKE_CURRENT_SOURCE_DIR}/browser/cmake/fetch_adult_domains.cmake")

add_executable(dalinira-adult-domain-snapshot-generator
  browser/native/core/adult_content_protection.cpp
  browser/native/tools/adult_domain_snapshot_generator.cpp)
target_link_libraries(dalinira-adult-domain-snapshot-generator PRIVATE Qt6::Core Qt6::Network)
target_include_directories(dalinira-adult-domain-snapshot-generator PRIVATE browser/native)

if(DALINIRA_ADULT_DOMAINS_SOURCE STREQUAL DALINIRA_ADULT_DOMAINS_MANAGED_SOURCE)
  add_custom_target(dalinira-adult-domains-source
    COMMAND ${CMAKE_COMMAND}
      -DDALINIRA_ADULT_DOMAINS_OUTPUT=${DALINIRA_ADULT_DOMAINS_SOURCE}
      -DDALINIRA_ADULT_DOMAINS_URL=${DALINIRA_ADULT_DOMAINS_UPSTREAM_URL}
      -DDALINIRA_ADULT_DOMAINS_SHA256=${DALINIRA_ADULT_DOMAINS_UPSTREAM_SHA256}
      -P ${DALINIRA_ADULT_DOMAINS_FETCH_SCRIPT}
    BYPRODUCTS ${DALINIRA_ADULT_DOMAINS_SOURCE}
    DEPENDS ${DALINIRA_ADULT_DOMAINS_FETCH_SCRIPT}
    COMMENT "Obtaining pinned Adult Protection production source"
    VERBATIM)
else()
  if(NOT EXISTS "${DALINIRA_ADULT_DOMAINS_SOURCE}")
    message(FATAL_ERROR
      "Custom Adult Protection production source does not exist: ${DALINIRA_ADULT_DOMAINS_SOURCE}")
  endif()
  add_custom_target(dalinira-adult-domains-source)
endif()

add_custom_command(
  OUTPUT ${DALINIRA_ADULT_DOMAINS_SNAPSHOT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_CURRENT_BINARY_DIR}/generated/privacy
  COMMAND $<TARGET_FILE:dalinira-adult-domain-snapshot-generator>
    ${DALINIRA_ADULT_DOMAINS_SOURCE}
    ${DALINIRA_ADULT_DOMAINS_SNAPSHOT}
  DEPENDS
    dalinira-adult-domain-snapshot-generator
    ${DALINIRA_ADULT_DOMAINS_SOURCE}
  COMMENT "Generating and validating Adult Protection domain snapshot"
  VERBATIM)
add_custom_target(dalinira-adult-domain-snapshot
  DEPENDS ${DALINIRA_ADULT_DOMAINS_SNAPSHOT})
add_dependencies(dalinira-adult-domain-snapshot
  dalinira-adult-domains-source)

add_executable(dalinira-browser ${BROWSER_SOURCES})
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-browser dalinira-webkit-extension)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dalinira-browser PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

qt_add_resources(dalinira-browser "app_icons"
  PREFIX "/"
  BASE browser
  FILES
    browser/assets/icons/dalinira-browser-16.png
    browser/assets/icons/dalinira-browser-32.png
    browser/assets/icons/dalinira-browser-128.png
    browser/assets/icons/dalinira-browser-256.png
)

qt_add_resources(dalinira-browser "app_icons_alias"
  PREFIX "/icons"
  BASE browser/assets/icons
  FILES
    browser/assets/icons/dalinira-browser-256.png
    browser/assets/icons/dalinira-browser-128.png
)

qt_add_resources(dalinira-browser "side_widget_icons"
  PREFIX "/side-widget-icons"
  BASE browser/assets/side-widget-icons
  FILES
    browser/assets/side-widget-icons/deliblock.svg
    browser/assets/side-widget-icons/web.svg
    browser/assets/side-widget-icons/files.svg
    browser/assets/side-widget-icons/quick-listen.svg
    browser/assets/side-widget-icons/pulse.svg
    browser/assets/side-widget-icons/video-tools.svg
    browser/assets/side-widget-icons/video.svg
    browser/assets/side-widget-icons/music.svg
    browser/assets/side-widget-icons/gallery.svg
    browser/assets/side-widget-icons/sound-effects.svg
    browser/assets/side-widget-icons/eq-presets.svg
    browser/assets/side-widget-icons/visualizer.svg
    browser/assets/side-widget-icons/settings-gear.svg
    browser/assets/side-widget-icons/about.svg
)

qt_add_resources(dalinira-browser "browser_icons"
  PREFIX "/browser-icons"
  BASE browser/assets/browser-icons
  FILES
    browser/assets/browser-icons/startup.svg
    browser/assets/browser-icons/appearance.svg
    browser/assets/browser-icons/performance.svg
    browser/assets/browser-icons/content.svg
    browser/assets/browser-icons/privacy.svg
    browser/assets/browser-icons/search.svg
    browser/assets/browser-icons/password.svg
    browser/assets/browser-icons/bookmark.svg
    browser/assets/browser-icons/download.svg
    browser/assets/browser-icons/language.svg
    browser/assets/browser-icons/accessibility.svg
    browser/assets/browser-icons/settings.svg
    browser/assets/browser-icons/reset.svg
    browser/assets/browser-icons/info.svg
    browser/assets/browser-icons/new-tab.svg
    browser/assets/browser-icons/window.svg
    browser/assets/browser-icons/incognito.svg
    browser/assets/browser-icons/history.svg
    browser/assets/browser-icons/zoom.svg
    browser/assets/browser-icons/print.svg
    browser/assets/browser-icons/save.svg
    browser/assets/browser-icons/tools.svg
    browser/assets/browser-icons/help.svg
    browser/assets/browser-icons/exit.svg
    browser/assets/browser-icons/folder.svg
    browser/assets/browser-icons/trash.svg
    browser/assets/browser-icons/grid.svg
    browser/assets/browser-icons/clock.svg
    browser/assets/browser-icons/cards.svg
    browser/assets/browser-icons/video.svg
    browser/assets/browser-icons/music.svg
    browser/assets/browser-icons/play.svg
    browser/assets/browser-icons/clipboard.svg
    browser/assets/browser-icons/more.svg
    browser/assets/browser-icons/minimize.svg
    browser/assets/browser-icons/maximize.svg
    browser/assets/browser-icons/restore.svg
    browser/assets/browser-icons/close.svg
    browser/assets/browser-icons/audio.svg
    browser/assets/browser-icons/memory.svg
    browser/assets/browser-icons/location.svg
    browser/assets/browser-icons/camera.svg
    browser/assets/browser-icons/microphone.svg
    browser/assets/browser-icons/notification.svg
    browser/assets/browser-icons/cookie.svg
    browser/assets/browser-icons/javascript.svg
    browser/assets/browser-icons/image.svg
    browser/assets/browser-icons/popup.svg
    browser/assets/browser-icons/chevron-right.svg
    browser/assets/browser-icons/chevron-up.svg
    browser/assets/browser-icons/find-in-page.svg
    browser/assets/browser-icons/mail.svg
    browser/assets/browser-icons/keyboard.svg
    browser/assets/browser-icons/chevron-down.svg
    browser/assets/browser-icons/fonts.svg
    browser/assets/browser-icons/mouse.svg
    browser/assets/browser-icons/pdf.svg
    browser/assets/browser-icons/location-slash.svg
    browser/assets/browser-icons/camera-slash.svg
    browser/assets/browser-icons/microphone-slash.svg
    browser/assets/browser-icons/notification-slash.svg
    browser/assets/browser-icons/arrow-left.svg
    browser/assets/browser-icons/javascript-slash.svg
    browser/assets/browser-icons/image-slash.svg
    browser/assets/browser-icons/popup-slash.svg
    browser/assets/browser-icons/protected-content.svg
    browser/assets/browser-icons/insecure-content.svg
    browser/assets/browser-icons/site-data.svg
    browser/assets/browser-icons/js-optimize.svg
    browser/assets/browser-icons/fullscreen.svg
    browser/assets/browser-icons/tune.svg
    browser/assets/browser-icons/chromium.svg
    browser/assets/browser-icons/webkit.svg
)

qt_add_resources(dalinira-browser "search_engine_icons"
  PREFIX "/search-engines"
  BASE browser/assets/new-tab
  FILES
    browser/assets/new-tab/google.ico
    browser/assets/new-tab/duckduckgo.ico
    browser/assets/new-tab/startpage.ico
    browser/assets/new-tab/mojeek.ico
)

qt_add_resources(dalinira-browser "new_tab_asset_fallbacks"
  PREFIX "/new-tab"
  BASE browser/assets/new-tab
  FILES
    browser/assets/new-tab/dalinira-flow-blue.png
)

qt_add_resources(dalinira-browser "translate_scripts"
  PREFIX "/translate"
  BASE browser/native/translate/resources
  FILES
    browser/native/translate/resources/translate_engine.js
)

qt_add_resources(dalinira-browser "i18n_resources"
  PREFIX "/i18n"
  BASE i18n
  FILES
    i18n/languages.json
    i18n/translations/en.json
    i18n/translations/tr.json
    i18n/translations/ar.json
    i18n/translations/fr.json
)

set_source_files_properties(${DALINIRA_ADULT_DOMAINS_SNAPSHOT} PROPERTIES
  GENERATED TRUE
  QT_RESOURCE_ALIAS adult_domains.bin)
qt_add_resources(dalinira-browser "privacy_resources"
  PREFIX "/privacy"
  FILES
    ${DALINIRA_ADULT_DOMAINS_SNAPSHOT}
    browser/resources/privacy/adult_allowlist.txt
)

target_link_libraries(dalinira-browser PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::WebEngineWidgets
  Qt6::WebChannel
  Qt6::Network
  Qt6::Svg
  Qt6::Concurrent
  Qt6::DBus
  OpenSSL::Crypto
  SQLite3::SQLite3
)

if(DALINIRA_BUILD_WEBKIT)
  target_link_libraries(dalinira-browser PRIVATE ${WEBKIT_LIBRARIES} ${X11_LIBRARIES})
  target_include_directories(dalinira-browser PRIVATE ${WEBKIT_INCLUDE_DIRS} ${X11_INCLUDE_DIRS})
endif()

target_compile_definitions(dalinira-browser PRIVATE
  DALINIRA_BROWSER_POLICY_PATH="${DALI_POLICY_JSON}"
  DALINIRA_BROWSER_VERSION="${PROJECT_VERSION}"
  DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH="dali/web-output-audiophile.generated.js"
  DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH="dali/web-equalizer-32.generated.js"
  DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH="dali/web-dynamic-compressor.generated.js"
  DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH="dali/web-limiter.generated.js"
  DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH="dali/web-bass-enhancer.generated.js"
  DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH="dali/web-auto-gain.generated.js"
)

add_custom_target(dalinira-eq-presets-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/eq-presets
    ${CMAKE_CURRENT_BINARY_DIR}/eq-presets
  VERBATIM
)

add_custom_target(dalinira-privacy-resources-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_CURRENT_BINARY_DIR}/resources/privacy
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${DALINIRA_ADULT_DOMAINS_SNAPSHOT}
    ${CMAKE_CURRENT_BINARY_DIR}/resources/privacy/adult_domains.bin
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/privacy/adult_allowlist.txt
    ${CMAKE_CURRENT_BINARY_DIR}/resources/privacy/adult_allowlist.txt
  DEPENDS dalinira-adult-domain-snapshot
  VERBATIM
)

add_dependencies(dalinira-browser
  dalinira-browser-policy
  dalinira-web-output-dali
  dalinira-eq-presets-runtime
  dalinira-privacy-resources-runtime
  dalinira-adult-domain-snapshot
)
if(TARGET dalinira-webengine-process)
  add_dependencies(dalinira-browser dalinira-webengine-process)
endif()

set_target_properties(dalinira-browser PROPERTIES
  OUTPUT_NAME "dalinira-browser"
)

# -------------------------------------------------------------
# Unit Tests
# -------------------------------------------------------------
enable_testing()

# Tests that open real windows run through this gate (see DALINIRA_GUI_TESTS
# below): under a plain `ctest` they report "Skipped" instead of opening
# windows on the developer's desktop. scripts/safe-gui-test.sh and CI (on a
# virtual display) enable them with DALINIRA_GUI_TESTS=1.
set(DALINIRA_GUI_GATE ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gui-test-gate.sh)

add_executable(dalinira-browser-policy-test
  browser/native/tests/browser_policy_test.cpp
  browser/native/core/browser_policy.cpp
)
target_link_libraries(dalinira-browser-policy-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
target_compile_definitions(dalinira-browser-policy-test PRIVATE DALINIRA_BROWSER_POLICY_PATH="${DALI_POLICY_JSON}")
add_dependencies(dalinira-browser-policy-test dalinira-browser-policy)
add_test(NAME browser-policy-test COMMAND dalinira-browser-policy-test)

add_executable(dalinira-browser-security-utils-test
  browser/native/tests/security_utils_test.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-browser-security-utils-test PRIVATE Qt6::Core)
add_test(NAME browser-security-utils COMMAND dalinira-browser-security-utils-test)

add_executable(dalinira-browser-feedback-system-test
  browser/native/tests/feedback_system_test.cpp
  browser/native/settings/feedback_links.cpp
)
target_link_libraries(dalinira-browser-feedback-system-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(dalinira-browser-feedback-system-test PRIVATE
  DALINIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME browser-feedback-system COMMAND dalinira-browser-feedback-system-test)

add_executable(dalinira-browser-web-engine-hardware-acceleration-test
  browser/native/tests/web_engine_hardware_acceleration_test.cpp
  browser/native/core/web_engine_hardware_acceleration.cpp
)
target_link_libraries(dalinira-browser-web-engine-hardware-acceleration-test PRIVATE Qt6::Core)
add_test(NAME browser-web-engine-hardware-acceleration COMMAND dalinira-browser-web-engine-hardware-acceleration-test)

add_executable(dalinira-browser-web-engine-memory-policy-test
  browser/native/tests/web_engine_memory_policy_test.cpp
  browser/native/core/web_engine_memory_policy.cpp
)
target_link_libraries(dalinira-browser-web-engine-memory-policy-test PRIVATE Qt6::Core)
add_test(NAME browser-web-engine-memory-policy COMMAND dalinira-browser-web-engine-memory-policy-test)

add_executable(dalinira-browser-eq-preset-repository-test
  browser/native/tests/eq_preset_repository_test.cpp
  browser/native/eq/eq_preset_repository.cpp
)
target_link_libraries(dalinira-browser-eq-preset-repository-test PRIVATE Qt6::Core)
add_dependencies(dalinira-browser-eq-preset-repository-test dalinira-eq-presets-runtime)
add_test(NAME browser-eq-preset-repository COMMAND dalinira-browser-eq-preset-repository-test)

add_executable(dalinira-browser-web-audio-effects-test
  browser/native/tests/web_audio_effects_controller_test.cpp
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/web_audio_effects_controller.cpp
  browser/native/engine/engine_view.cpp
  browser/native/audio/audio_effects_page.cpp
  browser/native/eq/eq_preset_repository.cpp
  browser/native/eq/eq_preset_page.cpp
)
target_link_libraries(dalinira-browser-web-audio-effects-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets Qt6::Svg
)
target_compile_definitions(dalinira-browser-web-audio-effects-test PRIVATE
  DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH="dali/web-output-audiophile.generated.js"
  DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH="dali/web-equalizer-32.generated.js"
  DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH="dali/web-dynamic-compressor.generated.js"
  DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH="dali/web-limiter.generated.js"
  DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH="dali/web-bass-enhancer.generated.js"
  DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH="dali/web-auto-gain.generated.js"
)
add_dependencies(dalinira-browser-web-audio-effects-test dalinira-web-output-dali dalinira-eq-presets-runtime)
add_test(NAME browser-web-audio-effects COMMAND dalinira-browser-web-audio-effects-test)
set_tests_properties(browser-web-audio-effects PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu"
  TIMEOUT 60
)

# The shared Web Audio effects engine without pages or engine processes.
add_executable(dalinira-browser-web-audio-engine-unit-test
  browser/native/tests/web_audio_engine_unit_test.cpp
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/web_audio_effects_controller.cpp
  browser/native/engine/engine_view.cpp
)
target_link_libraries(dalinira-browser-web-audio-engine-unit-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets
)
target_compile_definitions(dalinira-browser-web-audio-engine-unit-test PRIVATE
  DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH="dali/web-output-audiophile.generated.js"
  DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH="dali/web-equalizer-32.generated.js"
  DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH="dali/web-dynamic-compressor.generated.js"
  DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH="dali/web-limiter.generated.js"
  DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH="dali/web-bass-enhancer.generated.js"
  DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH="dali/web-auto-gain.generated.js"
)
add_dependencies(dalinira-browser-web-audio-engine-unit-test dalinira-web-output-dali)
add_test(NAME browser-web-audio-engine-unit COMMAND dalinira-browser-web-audio-engine-unit-test)
set_tests_properties(browser-web-audio-engine-unit PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

# Load test of the shared Web Audio effects engine: several feed-like pages,
# offscreen and muted, local silent media (no GUI, no audible output).
add_executable(dalinira-browser-web-audio-engine-stress-test
  browser/native/tests/web_audio_engine_stress_test.cpp
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/web_audio_effects_controller.cpp
  browser/native/engine/engine_view.cpp
)
target_include_directories(dalinira-browser-web-audio-engine-stress-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/browser/native)
target_link_libraries(dalinira-browser-web-audio-engine-stress-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets
)
target_compile_definitions(dalinira-browser-web-audio-engine-stress-test PRIVATE
  DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH="dali/web-output-audiophile.generated.js"
  DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH="dali/web-equalizer-32.generated.js"
  DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH="dali/web-dynamic-compressor.generated.js"
  DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH="dali/web-limiter.generated.js"
  DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH="dali/web-bass-enhancer.generated.js"
  DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH="dali/web-auto-gain.generated.js"
)
add_dependencies(dalinira-browser-web-audio-engine-stress-test dalinira-web-output-dali)
add_test(NAME browser-web-audio-engine-stress COMMAND dalinira-browser-web-audio-engine-stress-test)
set_tests_properties(browser-web-audio-engine-stress PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu --mute-audio"
  TIMEOUT 120
)

# A test-only system-WebKit build refuses to install (see above), so there is
# no installed layout to check.
if(NOT DALINIRA_SYSTEM_WEBKIT_TEST_BUILD)
  add_test(NAME browser-installed-package-layout
    COMMAND ${CMAKE_COMMAND}
      -DDALINIRA_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DDALINIRA_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}
      -DDALINIRA_EXPECT_WEBKIT=$<BOOL:${DALINIRA_BUILD_WEBKIT}>
      "-DDALINIRA_EXPECT_WEBKIT_RUNTIME=$<AND:$<BOOL:${DALINIRA_BUILD_WEBKIT}>,$<BOOL:${DALINIRA_WEBKIT_RUNTIME_SDK}>>"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/browser/cmake/verify_install_layout.cmake
  )
  set_tests_properties(browser-installed-package-layout PROPERTIES TIMEOUT 90)
endif()

add_executable(dalinira-browser-credential-vault-test
  browser/native/tests/credential_vault_test.cpp
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
)
target_link_libraries(dalinira-browser-credential-vault-test PRIVATE Qt6::Widgets Qt6::DBus OpenSSL::Crypto)
add_test(NAME browser-credential-vault-test COMMAND dalinira-browser-credential-vault-test)

add_executable(dalinira-browser-password-autofill-test
  browser/native/tests/password_autofill_test.cpp
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
  browser/native/passwords/credential_autofill_controller.cpp
  browser/native/passwords/credential_save_bubble.cpp
  browser/native/passwords/vault_unlock_dialog.cpp
  browser/native/core/browser_icons.cpp
  browser/native/core/search_engine_definition.cpp
)
target_link_libraries(dalinira-browser-password-autofill-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::WebChannel Qt6::Concurrent Qt6::Svg Qt6::DBus OpenSSL::Crypto)
qt_add_resources(dalinira-browser-password-autofill-test "password_autofill_icons"
    PREFIX "/browser-icons"
    BASE_DIR "browser/assets/browser-icons"
    FILES
    browser/assets/browser-icons/password.svg
)
add_test(NAME browser-password-autofill-test COMMAND dalinira-browser-password-autofill-test)
set_tests_properties(browser-password-autofill-test PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;DALINIRA_SLOW_WEBENGINE_CI=1"
  TIMEOUT 180)

add_executable(dalinira-password-manager-page-test
  browser/native/tests/password_manager_page_test.cpp
  browser/native/passwords/password_manager_page.cpp
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
)
target_link_libraries(dalinira-password-manager-page-test PRIVATE Qt6::Widgets Qt6::WebEngineCore Qt6::Concurrent Qt6::DBus OpenSSL::Crypto)
add_test(NAME browser-password-manager-page-test COMMAND dalinira-password-manager-page-test)
set_tests_properties(browser-password-manager-page-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(dalinira-browser-song-finder-test
  browser/native/tests/song_finder_test.cpp
  browser/native/pulse/song_finder_settings.cpp
  browser/native/audio/audio_device_manager.cpp
  browser/native/pulse/song_fingerprint.cpp
  browser/native/audio/audio_capture_service.cpp
  browser/native/pulse/song_recognition_service.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-browser-song-finder-test PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)
add_test(NAME browser-song-finder COMMAND dalinira-browser-song-finder-test)

add_executable(dalinira-browser-media-download-service-test
  browser/native/tests/media_download_service_test.cpp
  browser/native/downloads/media_download_service.cpp
  browser/native/downloads/yt_dlp_update_manager.cpp
  browser/native/core/adult_content_protection.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-browser-media-download-service-test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME browser-media-download-service COMMAND dalinira-browser-media-download-service-test)

add_executable(dalinira-browser-general-download-manager-test
  browser/native/tests/general_download_manager_test.cpp
  browser/native/downloads/general_download_manager.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-browser-general-download-manager-test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME browser-general-download-manager COMMAND dalinira-browser-general-download-manager-test)

add_executable(dalinira-browser-new-tab-asset-resolver-test
  browser/native/tests/new_tab_asset_resolver_test.cpp
  browser/native/newtab/new_tab_asset_resolver.cpp
)
target_link_libraries(dalinira-browser-new-tab-asset-resolver-test PRIVATE Qt6::Core)
add_test(NAME browser-new-tab-asset-resolver COMMAND dalinira-browser-new-tab-asset-resolver-test)

add_executable(dalinira-browser-download-ui-model-test
  browser/native/tests/download_ui_model_test.cpp
)
target_link_libraries(dalinira-browser-download-ui-model-test PRIVATE Qt6::Core)
add_test(NAME browser-download-ui-model COMMAND dalinira-browser-download-ui-model-test)

add_executable(dalinira-browser-local-media-routing-test
  browser/native/tests/local_media_routing_test.cpp
  browser/native/downloads/local_media_routing.cpp)
target_link_libraries(dalinira-browser-local-media-routing-test PRIVATE Qt6::Core)
add_test(NAME browser-local-media-routing COMMAND dalinira-browser-local-media-routing-test)

add_executable(dalinira-browser-webkit-download-parity-test
  browser/native/tests/webkit_download_parity_test.cpp
  browser/native/downloads/general_download_manager.cpp
  browser/native/downloads/media_platform_registry.cpp
  browser/native/downloads/media_page_url_resolver.cpp
  browser/native/downloads/download_desktop_actions.cpp
  browser/native/downloads/local_media_routing.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-browser-webkit-download-parity-test PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus)
add_test(NAME browser-webkit-download-parity COMMAND dalinira-browser-webkit-download-parity-test)
set_tests_properties(browser-webkit-download-parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(dalinira-browser-download-gui-parity-test
  browser/native/tests/download_gui_parity_test.cpp
  browser/native/downloads/download_desktop_actions.cpp
  browser/native/downloads/local_media_routing.cpp
  browser/native/i18n/language_manager.cpp
)
target_link_libraries(dalinira-browser-download-gui-parity-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)

add_executable(dalinira-browser-application-identity-test
  browser/native/tests/application_identity_test.cpp
  browser/native/core/application_identity.cpp)
target_compile_definitions(dalinira-browser-application-identity-test PRIVATE
  DALINIRA_DESKTOP_FILE="${CMAKE_CURRENT_BINARY_DIR}/dalinira.desktop")
target_link_libraries(dalinira-browser-application-identity-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME browser-application-identity COMMAND dalinira-browser-application-identity-test)
set_tests_properties(browser-application-identity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The keyboard shortcut registry and its persistence, without any window.
add_executable(dalinira-browser-keyboard-shortcut-service-test
  browser/native/tests/keyboard_shortcut_service_test.cpp
  browser/native/core/keyboard_shortcut_service.cpp)
target_compile_definitions(dalinira-browser-keyboard-shortcut-service-test PRIVATE
  DALINIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dalinira-browser-keyboard-shortcut-service-test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME browser-keyboard-shortcut-service COMMAND dalinira-browser-keyboard-shortcut-service-test)
set_tests_properties(browser-keyboard-shortcut-service PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(dalinira-browser-media-platform-registry-test
  browser/native/tests/media_platform_registry_test.cpp
  browser/native/downloads/media_platform_registry.cpp
)
target_link_libraries(dalinira-browser-media-platform-registry-test PRIVATE Qt6::Core)
add_test(NAME browser-media-platform-registry COMMAND dalinira-browser-media-platform-registry-test)

add_executable(dalinira-browser-media-page-url-resolver-test
  browser/native/tests/media_page_url_resolver_test.cpp
  browser/native/downloads/media_page_url_resolver.cpp
)
target_link_libraries(dalinira-browser-media-page-url-resolver-test PRIVATE
  Qt6::Core Qt6::Widgets Qt6::WebEngineCore Qt6::WebEngineWidgets)
add_test(NAME browser-media-page-url-resolver COMMAND dalinira-browser-media-page-url-resolver-test)
set_tests_properties(browser-media-page-url-resolver PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu"
  TIMEOUT 30)

add_executable(dalinira-browser-desktop-tab-system-test
  browser/native/tests/desktop_tab_system_test.cpp
  browser/native/desktop_tabs/tab_appearance.cpp
  browser/native/desktop_tabs/tab_layout_model.cpp
  browser/native/desktop_tabs/tab_strip_animator.cpp
  browser/native/desktop_tabs/tab_strip_widget.cpp
  browser/native/desktop_tabs/tab_group_model.cpp
  browser/native/desktop_tabs/tab_drag_session.cpp
  browser/native/desktop_tabs/tab_window_registry.cpp
  browser/native/desktop_tabs/detached_tab_window_controller.cpp
  browser/native/desktop_tabs/tab_drag_controller.cpp
  browser/native/desktop_tabs/platform/tab_drag_platform.cpp
)
target_link_libraries(dalinira-browser-desktop-tab-system-test PRIVATE Qt6::Widgets Qt6::Gui)
add_test(NAME browser-desktop-tab-system COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-desktop-tab-system-test>)
set_tests_properties(browser-desktop-tab-system PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-tab-drag-controller-test
  browser/native/tests/tab_drag_controller_test.cpp
  browser/native/desktop_tabs/tab_appearance.cpp
  browser/native/desktop_tabs/tab_layout_model.cpp
  browser/native/desktop_tabs/tab_strip_animator.cpp
  browser/native/desktop_tabs/tab_strip_widget.cpp
  browser/native/desktop_tabs/tab_group_model.cpp
  browser/native/desktop_tabs/tab_drag_session.cpp
  browser/native/desktop_tabs/tab_window_registry.cpp
  browser/native/desktop_tabs/detached_tab_window_controller.cpp
  browser/native/desktop_tabs/tab_drag_controller.cpp
  browser/native/desktop_tabs/platform/tab_drag_platform.cpp
)
target_link_libraries(dalinira-browser-tab-drag-controller-test PRIVATE Qt6::Widgets Qt6::Gui)
add_test(NAME browser-tab-drag-controller COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-tab-drag-controller-test>)
set_tests_properties(browser-tab-drag-controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-critical-tab-drag-scenario-test
  browser/native/tests/critical_tab_drag_scenario_test.cpp
  browser/native/desktop_tabs/tab_appearance.cpp
  browser/native/desktop_tabs/tab_layout_model.cpp
  browser/native/desktop_tabs/tab_strip_animator.cpp
  browser/native/desktop_tabs/tab_strip_widget.cpp
  browser/native/desktop_tabs/tab_group_model.cpp
  browser/native/desktop_tabs/tab_drag_session.cpp
  browser/native/desktop_tabs/tab_window_registry.cpp
  browser/native/desktop_tabs/detached_tab_window_controller.cpp
  browser/native/desktop_tabs/tab_drag_controller.cpp
  browser/native/desktop_tabs/platform/tab_drag_platform.cpp
)
target_link_libraries(dalinira-browser-critical-tab-drag-scenario-test PRIVATE Qt6::Widgets Qt6::Gui)
add_test(NAME browser-critical-tab-drag-scenario COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-critical-tab-drag-scenario-test>)

add_executable(dalinira-browser-runtime-feature-integration-test
  ${I18N_SOURCES}
  browser/native/tests/runtime_feature_integration_test.cpp
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/web_audio_effects_controller.cpp
  browser/native/engine/engine_view.cpp
  browser/native/blocker/dalinira_blocker_service.cpp
  browser/native/blocker/dalinira_blocker_engine.cpp
  browser/native/blocker/dalinira_blocker_list_manager.cpp
  browser/native/blocker/dalinira_blocker_filter_updates.cpp
  browser/native/blocker/dalinira_blocker_redirect_resources.cpp
  browser/native/blocker/dalinira_blocker_element_picker.cpp
  browser/native/blocker/dalinira_blocker_request_interceptor.cpp
  browser/native/blocker/dalinira_blocker_settings.cpp
  browser/native/pulse/song_recognition_service.cpp
  browser/native/pulse/song_finder_settings.cpp
  browser/native/audio/audio_capture_service.cpp
  browser/native/audio/audio_device_manager.cpp
  browser/native/pulse/song_fingerprint.cpp
  browser/native/newtab/new_tab_scheme.cpp
  browser/native/newtab/new_tab_asset_resolver.cpp
  browser/native/newtab/new_tab_html.cpp
  browser/native/core/security_utils.cpp
  browser/native/core/adult_content_protection.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/browser_icons.cpp
  browser/native/core/search_suggestion_service.cpp
  browser/native/core/address_input_resolver.cpp
  browser/native/downloads/media_download_service.cpp
  browser/native/downloads/yt_dlp_update_manager.cpp
  browser/native/blocker/dalinira_blocker_shield_button.cpp
)
target_link_libraries(dalinira-browser-runtime-feature-integration-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Concurrent Qt6::Svg)
target_compile_definitions(dalinira-browser-runtime-feature-integration-test PRIVATE
  DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH="dali/web-output-audiophile.generated.js"
  DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH="dali/web-equalizer-32.generated.js"
  DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH="dali/web-dynamic-compressor.generated.js"
  DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH="dali/web-limiter.generated.js"
  DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH="dali/web-bass-enhancer.generated.js"
  DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH="dali/web-auto-gain.generated.js"
)
add_test(NAME browser-runtime-feature-integration COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-runtime-feature-integration-test>)

add_executable(dalinira-browser-phase2a-policy-test
  browser/native/tests/phase2a_policy_test.cpp
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/audio_device_manager.cpp
  browser/native/core/browser_permission_policy.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-browser-phase2a-policy-test PRIVATE Qt6::Core Qt6::WebEngineWidgets)
add_test(NAME browser-phase2a-policy COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-phase2a-policy-test>)

add_executable(dalinira-browser-tab-scheduler-retry-test
  browser/native/tests/tab_scheduler_retry_test.cpp
  browser/native/desktop_tabs/tab_manager.cpp
  browser/native/desktop_tabs/tab_performance_manager.cpp
  browser/native/desktop_tabs/tab_lifecycle_adapter.cpp
  browser/native/desktop_tabs/tab_resource_model.cpp
  browser/native/engine/engine_view.cpp
  browser/native/desktop_tabs/system_memory_pressure_monitor.cpp
)
target_link_libraries(dalinira-browser-tab-scheduler-retry-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
add_test(NAME browser-tab-scheduler-retry COMMAND dalinira-browser-tab-scheduler-retry-test)
set_tests_properties(browser-tab-scheduler-retry PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-tab-hover-card-test
  browser/native/tests/tab_hover_card_test.cpp
  browser/native/desktop_tabs/tab_hover_card.cpp
  browser/native/desktop_tabs/tab_resource_model.cpp
  browser/native/desktop_tabs/tab_manager.cpp
  browser/native/desktop_tabs/tab_performance_manager.cpp
  browser/native/desktop_tabs/tab_lifecycle_adapter.cpp
  browser/native/desktop_tabs/system_memory_pressure_monitor.cpp
  browser/native/engine/engine_view.cpp
  ${I18N_SOURCES}
  browser/native/core/browser_icons.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/search_suggestion_service.cpp
  browser/native/core/address_input_resolver.cpp
)
target_link_libraries(dalinira-browser-tab-hover-card-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::Svg)
add_test(NAME browser-tab-hover-card COMMAND dalinira-browser-tab-hover-card-test)
set_tests_properties(browser-tab-hover-card PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-address-input-resolver-test
  browser/native/core/search_engine_definition.cpp
  browser/native/tests/address_input_resolver_test.cpp
  browser/native/core/address_input_resolver.cpp
)
target_link_libraries(dalinira-browser-address-input-resolver-test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME browser-address-input-resolver COMMAND dalinira-browser-address-input-resolver-test)

# Google sign-in gets the Chromium engine's own User-Agent back.
add_executable(dalinira-browser-sign-in-compatibility-test
  browser/native/tests/sign_in_compatibility_test.cpp)
target_link_libraries(dalinira-browser-sign-in-compatibility-test PRIVATE Qt6::Core)
add_test(NAME browser-sign-in-compatibility COMMAND dalinira-browser-sign-in-compatibility-test)

add_executable(dalinira-browser-omnibox-navigation-integration-test
  browser/native/core/search_engine_definition.cpp
  browser/native/tests/omnibox_navigation_integration_test.cpp
  browser/native/core/address_input_resolver.cpp
)
target_compile_definitions(dalinira-browser-omnibox-navigation-integration-test PRIVATE DALINIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dalinira-browser-omnibox-navigation-integration-test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME browser-omnibox-navigation-integration COMMAND dalinira-browser-omnibox-navigation-integration-test)

add_executable(dalinira-browser-smart-omnibox-phase2-test
  browser/native/core/search_engine_definition.cpp
  browser/native/tests/smart_omnibox_phase2_test.cpp
  browser/native/core/address_input_resolver.cpp
  browser/native/core/domain_normalizer.cpp
  browser/native/core/history_candidate_provider.cpp
  browser/native/core/bookmark_candidate_provider.cpp
  browser/native/core/frequent_sites_candidate_provider.cpp
  browser/native/core/composite_navigation_candidate_provider.cpp
)
target_link_libraries(dalinira-browser-smart-omnibox-phase2-test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME browser-smart-omnibox-phase2 COMMAND dalinira-browser-smart-omnibox-phase2-test)

add_executable(smart_omnibox_phase2_1_test
  ${I18N_SOURCES}
  browser/native/tests/smart_omnibox_phase2_1_test.cpp
  browser/native/core/browser_profile_service.cpp
  browser/native/engine/engine_backend.cpp
  browser/native/onboarding/onboarding_manager.cpp
  browser/native/downloads/general_download_manager.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/search_suggestion_service.cpp
  browser/native/core/address_input_resolver.cpp
  browser/native/core/browser_policy.cpp
  browser/native/core/security_utils.cpp
  browser/native/core/adult_content_protection.cpp
  browser/native/newtab/new_tab_html.cpp
  browser/native/newtab/new_tab_asset_resolver.cpp
  browser/native/newtab/new_tab_scheme.cpp
  browser/native/newtab/new_tab_background_store.cpp
  browser/native/core/browser_icons.cpp
  browser/native/blocker/dalinira_blocker_settings.cpp
  browser/native/blocker/dalinira_blocker_engine.cpp
  browser/native/blocker/dalinira_blocker_list_manager.cpp
  browser/native/blocker/dalinira_blocker_filter_updates.cpp
  browser/native/blocker/dalinira_blocker_redirect_resources.cpp
  browser/native/blocker/dalinira_blocker_element_picker.cpp
  browser/native/blocker/dalinira_blocker_request_interceptor.cpp
  browser/native/blocker/dalinira_blocker_service.cpp
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
  browser/native/translate/translate_service.cpp
  browser/native/translate/translation_secret_store.cpp
  browser/native/translate/providers/libretranslate_provider.cpp
  browser/native/translate/providers/deepl_provider.cpp
  browser/native/translate/providers/google_cloud_provider.cpp
  browser/native/translate/providers/google_gtx_provider.cpp
)
target_link_libraries(smart_omnibox_phase2_1_test PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::WebEngineWidgets
  Qt6::Network
  Qt6::Concurrent
  Qt6::DBus
  Qt6::Svg
  OpenSSL::Crypto
)
add_test(NAME browser-newtab-shortcuts-storage COMMAND smart_omnibox_phase2_1_test --newtab-shortcuts-storage)
add_test(NAME browser-newtab-shortcuts COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:smart_omnibox_phase2_1_test> --newtab-shortcuts)
set_tests_properties(browser-newtab-shortcuts PROPERTIES LABELS "gui" TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
add_test(NAME smart_omnibox_phase2_1_test COMMAND smart_omnibox_phase2_1_test)
set_tests_properties(smart_omnibox_phase2_1_test PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-phase22d-test
  ${I18N_SOURCES}
  browser/native/tests/phase22d_test.cpp
  browser/native/newtab/new_tab_html.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/search_suggestion_service.cpp
  browser/native/core/address_input_resolver.cpp
  browser/native/blocker/dalinira_blocker_settings.cpp
  browser/native/blocker/dalinira_blocker_engine.cpp
  browser/native/blocker/dalinira_blocker_list_manager.cpp
  browser/native/blocker/dalinira_blocker_filter_updates.cpp
  browser/native/blocker/dalinira_blocker_redirect_resources.cpp
  browser/native/blocker/dalinira_blocker_element_picker.cpp
  browser/native/blocker/dalinira_blocker_service.cpp
  browser/native/blocker/dalinira_blocker_request_interceptor.cpp
  browser/native/blocker/dalinira_blocker_shield_button.cpp
  browser/native/core/adult_content_protection.cpp
  browser/native/core/security_utils.cpp
)
target_link_libraries(dalinira-phase22d-test PRIVATE Qt6::Core Qt6::Widgets Qt6::Network Qt6::WebEngineWidgets Qt6::Concurrent)
qt_add_resources(dalinira-phase22d-test "phase22d_test_side_widget_icons"
  PREFIX "/side-widget-icons"
  BASE browser/assets/side-widget-icons
  FILES
    browser/assets/side-widget-icons/deliblock.svg
)
add_test(NAME dalinira-phase22d COMMAND dalinira-phase22d-test)
set_tests_properties(dalinira-phase22d PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

# Release verification must execute assertions in the existing standalone tests.
get_property(DALINIRA_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(DALINIRA_TEST_TARGET IN LISTS DALINIRA_TEST_TARGETS)
  if(DALINIRA_TEST_TARGET MATCHES "[-_]test$")
    target_compile_options(${DALINIRA_TEST_TARGET} PRIVATE -UNDEBUG)
  endif()
endforeach()

# Exercise the actual native window and privileged New Tab bridge as well as
# the downloader's independent unit fixtures.
get_target_property(DALINIRA_PHASE22D_INTEGRATION_SOURCES dalinira-browser SOURCES)
list(REMOVE_ITEM DALINIRA_PHASE22D_INTEGRATION_SOURCES browser/native/main.cpp)
add_executable(dalinira-phase22d-integration-test
  ${DALINIRA_PHASE22D_INTEGRATION_SOURCES}
  browser/native/tests/phase22d_integration_test.cpp)
get_target_property(DALINIRA_PHASE22D_LIBRARIES dalinira-browser LINK_LIBRARIES)
get_target_property(DALINIRA_PHASE22D_DEFINITIONS dalinira-browser COMPILE_DEFINITIONS)
target_link_libraries(dalinira-phase22d-integration-test PRIVATE ${DALINIRA_PHASE22D_LIBRARIES})
target_compile_definitions(dalinira-phase22d-integration-test PRIVATE ${DALINIRA_PHASE22D_DEFINITIONS})
target_compile_options(dalinira-phase22d-integration-test PRIVATE -UNDEBUG)
add_dependencies(dalinira-phase22d-integration-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME dalinira-phase22d-integration COMMAND dalinira-phase22d-integration-test)
set(DALINIRA_PHASE22D_TEST_TIMEOUT 120 CACHE STRING "Phase22d integration test timeout in seconds")
set_tests_properties(dalinira-phase22d-integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT ${DALINIRA_PHASE22D_TEST_TIMEOUT})

get_target_property(DALINIRA_BROWSER_FULL_SOURCES dalinira-browser SOURCES)
list(REMOVE_ITEM DALINIRA_BROWSER_FULL_SOURCES browser/native/main.cpp)
add_executable(dalinira-browser-site-permission-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/site_permission_system_test.cpp)
get_target_property(DALINIRA_BROWSER_FULL_LIBS dalinira-browser LINK_LIBRARIES)
get_target_property(DALINIRA_BROWSER_FULL_DEFS dalinira-browser COMPILE_DEFINITIONS)
target_link_libraries(dalinira-browser-site-permission-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-site-permission-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
target_compile_options(dalinira-browser-site-permission-test PRIVATE -UNDEBUG)
add_dependencies(dalinira-browser-site-permission-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-site-permission COMMAND dalinira-browser-site-permission-test)
set_tests_properties(browser-site-permission PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

add_executable(dalinira-browser-context-menu-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/tab_and_link_context_menu_test.cpp)
target_link_libraries(dalinira-browser-context-menu-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-context-menu-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
target_compile_options(dalinira-browser-context-menu-test PRIVATE -UNDEBUG)
add_dependencies(dalinira-browser-context-menu-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-context-menu COMMAND dalinira-browser-context-menu-test)
set_tests_properties(browser-context-menu PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

add_executable(dalinira-adult-content-protection-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/adult_content_protection_test.cpp)
target_link_libraries(dalinira-adult-content-protection-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-adult-content-protection-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
target_compile_options(dalinira-adult-content-protection-test PRIVATE -UNDEBUG)
add_dependencies(dalinira-adult-content-protection-test dalinira-browser-policy dalinira-web-output-dali dalinira-privacy-resources-runtime)
add_test(NAME adult-content-protection-test COMMAND dalinira-adult-content-protection-test)
set_tests_properties(adult-content-protection-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

add_executable(dalinira-adult-content-benchmark
  browser/native/core/adult_content_protection.cpp
  browser/native/tests/adult_content_benchmark.cpp)
target_link_libraries(dalinira-adult-content-benchmark PRIVATE Qt6::Core Qt6::Network)
target_include_directories(dalinira-adult-content-benchmark PRIVATE browser/native)

add_executable(dalinira-adult-content-benchmark-release
  browser/native/core/adult_content_protection.cpp
  browser/native/tests/adult_content_benchmark.cpp)
target_link_libraries(dalinira-adult-content-benchmark-release PRIVATE Qt6::Core Qt6::Network)
target_include_directories(dalinira-adult-content-benchmark-release PRIVATE browser/native)
target_compile_options(dalinira-adult-content-benchmark-release PRIVATE -O3 -DNDEBUG)


# Developer memory profiler (docs/memory-profiling.md); built on request only.
add_executable(dalinira-memory-profile EXCLUDE_FROM_ALL
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tools/memory_profile.cpp)
target_link_libraries(dalinira-memory-profile PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-memory-profile PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-memory-profile dalinira-browser-policy dalinira-web-output-dali)
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-memory-profile dalinira-webkit-extension)
endif()

add_executable(dalinira-browser-step5-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/step5_essential_features_test.cpp)
target_link_libraries(dalinira-browser-step5-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-step5-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
target_compile_options(dalinira-browser-step5-test PRIVATE -UNDEBUG)
add_dependencies(dalinira-browser-step5-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-step5-essential-features COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-step5-test>)
# The WebKit phase opens three tabs at once (bookmark middle-click and open in
# new tab), each with its own web process.
set_tests_properties(browser-step5-essential-features PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;DALINIRA_GUI_MAX_WEB_PROCESSES=4" TIMEOUT 120)
add_test(NAME browser-secure-dns-configuration
  COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-step5-test> --secure-dns-only)
set_tests_properties(browser-secure-dns-configuration PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu"
  TIMEOUT 60)

set_tests_properties(smart_omnibox_phase2_1_test PROPERTIES TIMEOUT 90)

# Tab Sleep/Wake menu and hover-card memory on both engines (GUI tier). The
# renderer limit makes the Chromium phase's two tabs share one renderer.
add_executable(dalinira-browser-tab-memory-ux-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/tab_memory_ux_test.cpp)
target_link_libraries(dalinira-browser-tab-memory-ux-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-tab-memory-ux-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-tab-memory-ux-test dalinira-browser-policy dalinira-web-output-dali)
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-browser-tab-memory-ux-test dalinira-webkit-extension)
endif()
add_test(NAME browser-tab-memory-ux COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-tab-memory-ux-test>)
set_tests_properties(browser-tab-memory-ux PROPERTIES
  # Two WebKit tabs: two page processes, plus a site's service worker or a
  # cross-site swap overlapping briefly (safe-gui-test.sh reads this budget).
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;GDK_BACKEND=x11;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu --renderer-process-limit=1;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
  TIMEOUT 240)

# Short WebKit lifecycle regression (GUI tier): tabs opening and closing, a
# hidden background tab, sleep/wake, window close and engine shutdown with no
# WebKit process left or lost, in one window lifetime. It stands for the
# WebKitGTK 2.54.1 compositor crash fixed by the private runtime's patch; the
# patch's 500-lifetime stress validation is research only
# (docs/research/webkit-exit-crash/) and never part of a test tier.
if(DALINIRA_BUILD_WEBKIT)
  add_executable(dalinira-browser-webkit-lifecycle-test
    ${DALINIRA_BROWSER_FULL_SOURCES}
    browser/native/tests/webkit_lifecycle_regression_test.cpp)
  target_link_libraries(dalinira-browser-webkit-lifecycle-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
  if(DALINIRA_BROWSER_FULL_DEFS)
    target_compile_definitions(dalinira-browser-webkit-lifecycle-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
  endif()
  add_dependencies(dalinira-browser-webkit-lifecycle-test dalinira-browser-policy dalinira-web-output-dali
    dalinira-webkit-extension)
  add_test(NAME browser-webkit-lifecycle COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-webkit-lifecycle-test>)
  set_tests_properties(browser-webkit-lifecycle PROPERTIES
    # Three WebKit tabs at most, one web process each (safe-gui-test.sh reads this budget).
    ENVIRONMENT "QT_QPA_PLATFORM=xcb;GDK_BACKEND=x11;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
    TIMEOUT 120)
endif()

# Engine keys (browser/engine, --engine, workspace scopes) and their
# normalization, without any engine.
add_executable(dalinira-browser-engine-identity-test
  browser/native/tests/engine_identity_test.cpp)
target_link_libraries(dalinira-browser-engine-identity-test PRIVATE Qt6::Core Qt6::Widgets)
add_test(NAME browser-engine-identity COMMAND dalinira-browser-engine-identity-test)
set_tests_properties(browser-engine-identity PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

# Chromium and WebKit browsing workspaces (tabs, session, shortcut row) are
# independent; the bookmark library stays shared. One executable: the logic
# checks run in the default tier; the engine switch (one child process per
# engine, run one after another, as DaliNira relaunches) is a GUI-tier test.
add_executable(dalinira-browser-engine-workspace-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/engine_workspace_test.cpp)
target_link_libraries(dalinira-browser-engine-workspace-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-engine-workspace-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-engine-workspace-test dalinira-browser-policy dalinira-web-output-dali)
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-browser-engine-workspace-test dalinira-webkit-extension)
endif()
add_test(NAME browser-engine-workspace COMMAND dalinira-browser-engine-workspace-test --logic)
set_tests_properties(browser-engine-workspace PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 60)
add_test(NAME browser-engine-workspace-switch
  COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-engine-workspace-test> --gui)
set_tests_properties(browser-engine-workspace-switch PROPERTIES
  # One window at a time; the WebKit phases hold two tabs (two page
  # processes, plus a brief cross-site swap).
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;GDK_BACKEND=x11;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
  TIMEOUT 300)

# Windowing / input compatibility of both engines on X11, XWayland and native
# Wayland. The display plan logic runs in the default tier; the GUI tier runs
# one child process per engine, once with Qt on xcb and once on wayland (on
# Wayland sessions; WebKit then still runs through XWayland).
add_executable(dalinira-browser-window-platform-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/window_platform_test.cpp)
target_link_libraries(dalinira-browser-window-platform-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-window-platform-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
if(DALINIRA_BUILD_WEBKIT)
  # Real X key events into the WebKit surface (only while its focus is verified).
  pkg_check_modules(DALINIRA_XTST xtst)
  if(DALINIRA_XTST_FOUND)
    target_compile_definitions(dalinira-browser-window-platform-test PRIVATE DALINIRA_TEST_XTEST=1)
    target_include_directories(dalinira-browser-window-platform-test PRIVATE ${DALINIRA_XTST_INCLUDE_DIRS})
    target_link_libraries(dalinira-browser-window-platform-test PRIVATE ${DALINIRA_XTST_LIBRARIES})
  endif()
endif()
add_dependencies(dalinira-browser-window-platform-test dalinira-browser-policy dalinira-web-output-dali)
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-browser-window-platform-test dalinira-webkit-extension)
endif()
add_test(NAME browser-window-platform COMMAND dalinira-browser-window-platform-test --logic)
set_tests_properties(browser-window-platform PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
foreach(DALINIRA_QPA xcb wayland)
  add_test(NAME browser-window-platform-${DALINIRA_QPA}
    COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-window-platform-test> --gui)
  set_tests_properties(browser-window-platform-${DALINIRA_QPA} PROPERTIES
    # One window at a time; the WebKit phase holds up to three tabs.
    ENVIRONMENT "QT_QPA_PLATFORM=${DALINIRA_QPA};QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
    TIMEOUT 300)
endforeach()

# User-visible Chromium / WebKit parity in the real BrowserWindow: popups and
# window.opener, per-site "block scripts", fullscreen media, onboarding and
# runtime layout / theme switching. One GUI test per case; each runs a
# Chromium then a WebKit child and compares their results.
add_executable(dalinira-browser-engine-parity-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/engine_parity_test.cpp)
target_link_libraries(dalinira-browser-engine-parity-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-engine-parity-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
target_compile_definitions(dalinira-browser-engine-parity-test PRIVATE
  DALINIRA_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/browser/native/tests/fixtures/media")
add_dependencies(dalinira-browser-engine-parity-test dalinira-browser-policy dalinira-web-output-dali)
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-browser-engine-parity-test dalinira-webkit-extension)
endif()
set(DALINIRA_PARITY_CASES popup scripts media layout privacy network fingerprint audit about find shortcuts sidepanel)
# Real Qt key events for the shortcuts case's Chromium key path (optional).
find_package(Qt6 QUIET COMPONENTS Test)
if(TARGET Qt6::Test)
  target_compile_definitions(dalinira-browser-engine-parity-test PRIVATE DALINIRA_TEST_QTEST=1)
  target_link_libraries(dalinira-browser-engine-parity-test PRIVATE Qt6::Test)
endif()
if(DALINIRA_XTST_FOUND)
  target_compile_definitions(dalinira-browser-engine-parity-test PRIVATE DALINIRA_TEST_XTEST=1)
  target_include_directories(dalinira-browser-engine-parity-test PRIVATE ${DALINIRA_XTST_INCLUDE_DIRS})
  target_link_libraries(dalinira-browser-engine-parity-test PRIVATE ${DALINIRA_XTST_LIBRARIES})
  list(APPEND DALINIRA_PARITY_CASES translate)
endif()
foreach(DALINIRA_PARITY_CASE IN LISTS DALINIRA_PARITY_CASES)
  add_test(NAME browser-engine-parity-${DALINIRA_PARITY_CASE}
    COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-engine-parity-test> --gui ${DALINIRA_PARITY_CASE})
  set_tests_properties(browser-engine-parity-${DALINIRA_PARITY_CASE} PROPERTIES
    # One window at a time; popups share their opener's web process, the
    # script case swaps processes across two sites.
    ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
    TIMEOUT 330)
endforeach()
# The layout case again with Qt on native Wayland (Chromium; WebKit still runs
# through XWayland). Skipped outside a Wayland session.
add_test(NAME browser-engine-parity-layout-wayland
  COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-engine-parity-test> --gui layout)
set_tests_properties(browser-engine-parity-layout-wayland PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=wayland;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
  TIMEOUT 330)

# The shared Web Audio effects engine in the real BrowserWindow, Chromium then
# WebKit (one child process per engine, one after the other), same checks.
add_executable(dalinira-browser-web-audio-engine-gui-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/web_audio_engine_gui_test.cpp)
target_link_libraries(dalinira-browser-web-audio-engine-gui-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-web-audio-engine-gui-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-web-audio-engine-gui-test dalinira-browser-policy dalinira-web-output-dali)
if(TARGET dalinira-webkit-extension)
  add_dependencies(dalinira-browser-web-audio-engine-gui-test dalinira-webkit-extension)
endif()
add_test(NAME browser-web-audio-engine-gui
  COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-web-audio-engine-gui-test>)
set_tests_properties(browser-web-audio-engine-gui PROPERTIES
  # One window at a time; the WebKit phase has two media tabs.
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;GDK_BACKEND=x11;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
  TIMEOUT 300)

# Effect activation latency (Chromium then WebKit), from the same binary.
add_test(NAME browser-web-audio-activation-latency
  COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-web-audio-engine-gui-test> --latency)
set_tests_properties(browser-web-audio-activation-latency PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;GDK_BACKEND=x11;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;DALINIRA_GUI_MAX_WEB_PROCESSES=4"
  TIMEOUT 600)

add_executable(dalinira-language-manager-test
  ${I18N_SOURCES}
  browser/native/tests/language_manager_test.cpp
)
qt_add_resources(dalinira-language-manager-test "i18n_resources_test"
  PREFIX "/i18n"
  BASE i18n
  FILES
    i18n/languages.json
    i18n/translations/en.json
    i18n/translations/tr.json
    i18n/translations/ar.json
    i18n/translations/fr.json
)
target_link_libraries(dalinira-language-manager-test PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
)
target_compile_options(dalinira-language-manager-test PRIVATE -UNDEBUG)
add_test(NAME language-manager-test COMMAND dalinira-language-manager-test)
set_tests_properties(language-manager-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

# Standalone regression programs which predate CTest registration. Keep each
# executable independent so a failure identifies the affected subsystem.
add_executable(dalinira-browser-new-tab-background-store-test
  browser/native/tests/new_tab_background_store_test.cpp
  browser/native/newtab/new_tab_background_store.cpp)
target_link_libraries(dalinira-browser-new-tab-background-store-test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME browser-new-tab-background-store COMMAND dalinira-browser-new-tab-background-store-test)

add_executable(dalinira-browser-tab-throbber-test
  browser/native/tests/tab_throbber_test.cpp
  browser/native/desktop_tabs/tab_throbber.cpp)
target_link_libraries(dalinira-browser-tab-throbber-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
add_test(NAME browser-tab-throbber COMMAND dalinira-browser-tab-throbber-test)
set_tests_properties(browser-tab-throbber PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-tab-manager-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/tab_manager_test.cpp)
target_link_libraries(dalinira-browser-tab-manager-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-tab-manager-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-tab-manager-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-tab-manager COMMAND dalinira-browser-tab-manager-test)
set_tests_properties(browser-tab-manager PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-internal-tab-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/internal_tab_test.cpp)
target_link_libraries(dalinira-browser-internal-tab-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-internal-tab-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-internal-tab-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-internal-tab COMMAND dalinira-browser-internal-tab-test)
set_tests_properties(browser-internal-tab PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-onboarding-test
  browser/native/tests/onboarding_test.cpp
  browser/native/onboarding/browser_data_importer.cpp
  browser/native/onboarding/engine_step.cpp
  browser/native/onboarding/import_step.cpp
  browser/native/onboarding/layout_step.cpp
  browser/native/onboarding/search_privacy_step.cpp
  browser/native/onboarding/ready_step.cpp
  browser/native/onboarding/onboarding_manager.cpp
  browser/native/onboarding/onboarding_step.cpp
  browser/native/onboarding/onboarding_view.cpp
  browser/native/onboarding/welcome_step.cpp
  browser/native/onboarding/aurora_wave_background.cpp
  browser/native/onboarding/onboarding_window.cpp
  browser/native/layout/browser_layout.cpp
  browser/native/layout/browser_layout_controller.cpp
  browser/native/layout/browser_side_panel.cpp
  browser/native/core/application_identity.cpp
  browser/native/core/browser_icons.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/i18n/language_manager.cpp)
target_link_libraries(dalinira-browser-onboarding-test PRIVATE Qt6::Widgets Qt6::Svg SQLite3::SQLite3)
add_test(NAME browser-onboarding COMMAND dalinira-browser-onboarding-test)
set_tests_properties(browser-onboarding PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The auto-hiding side tab panel's states, without a web engine.
add_executable(dalinira-browser-side-panel-auto-hide-test
  browser/native/tests/side_panel_auto_hide_test.cpp
  browser/native/layout/browser_layout.cpp
  browser/native/layout/browser_layout_controller.cpp
  browser/native/layout/browser_side_panel.cpp
  browser/native/core/browser_icons.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/i18n/language_manager.cpp)
target_link_libraries(dalinira-browser-side-panel-auto-hide-test PRIVATE Qt6::Widgets Qt6::Svg)
add_test(NAME browser-side-panel-auto-hide COMMAND dalinira-browser-side-panel-auto-hide-test)
set_tests_properties(browser-side-panel-auto-hide PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(dalinira-browser-tab-performance-manager-test
  browser/native/tests/tab_performance_manager_test.cpp
  browser/native/desktop_tabs/tab_manager.cpp
  browser/native/desktop_tabs/tab_performance_manager.cpp
  browser/native/desktop_tabs/tab_lifecycle_adapter.cpp
  browser/native/desktop_tabs/tab_resource_model.cpp
  browser/native/engine/engine_view.cpp
  browser/native/desktop_tabs/system_memory_pressure_monitor.cpp)
target_link_libraries(dalinira-browser-tab-performance-manager-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
add_test(NAME browser-tab-performance-manager COMMAND dalinira-browser-tab-performance-manager-test)
set_tests_properties(browser-tab-performance-manager PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

# The shared tab sleeping policy both engines follow (engine-neutral, no GUI).
add_executable(dalinira-browser-tab-sleep-policy-test
  browser/native/tests/tab_sleep_policy_test.cpp
  browser/native/desktop_tabs/tab_manager.cpp
  browser/native/desktop_tabs/tab_performance_manager.cpp
  browser/native/desktop_tabs/tab_lifecycle_adapter.cpp
  browser/native/desktop_tabs/tab_resource_model.cpp
  browser/native/engine/engine_view.cpp
  browser/native/desktop_tabs/system_memory_pressure_monitor.cpp)
target_link_libraries(dalinira-browser-tab-sleep-policy-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
add_test(NAME browser-tab-sleep-policy COMMAND dalinira-browser-tab-sleep-policy-test)
set_tests_properties(browser-tab-sleep-policy PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 60)

add_executable(dalinira-browser-page-translator-test
  browser/native/tests/page_translator_test.cpp
  browser/native/translate/page_translator.cpp
  browser/native/translate/translate_service.cpp
  browser/native/translate/translation_secret_store.cpp
  browser/native/translate/language_detector.cpp
  browser/native/translate/providers/libretranslate_provider.cpp
  browser/native/translate/providers/deepl_provider.cpp
  browser/native/translate/providers/google_cloud_provider.cpp
  browser/native/translate/providers/google_gtx_provider.cpp
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
  browser/native/core/security_utils.cpp)
target_link_libraries(dalinira-browser-page-translator-test PRIVATE
  Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Concurrent Qt6::DBus OpenSSL::Crypto)
add_test(NAME browser-page-translator COMMAND dalinira-browser-page-translator-test)
set_tests_properties(browser-page-translator PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu"
  LABELS "gui;integration")

add_executable(dalinira-browser-state-transfer-test
  browser/native/tests/state_transfer_test.cpp)
target_link_libraries(dalinira-browser-state-transfer-test PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
add_test(NAME browser-state-transfer COMMAND dalinira-browser-state-transfer-test)
set_tests_properties(browser-state-transfer PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

add_executable(dalinira-browser-translate-service-test
  browser/native/tests/translate_service_test.cpp
  browser/native/translate/translate_service.cpp
  browser/native/translate/translation_secret_store.cpp
  browser/native/translate/language_detector.cpp
  browser/native/translate/providers/libretranslate_provider.cpp
  browser/native/translate/providers/deepl_provider.cpp
  browser/native/translate/providers/google_cloud_provider.cpp
  browser/native/translate/providers/google_gtx_provider.cpp
  browser/native/passwords/device_keyring.cpp
  browser/native/passwords/credential_vault.cpp
  browser/native/passwords/credential_vault_manager.cpp
  browser/native/core/security_utils.cpp)
target_link_libraries(dalinira-browser-translate-service-test PRIVATE
  Qt6::Core Qt6::Network Qt6::Concurrent Qt6::DBus OpenSSL::Crypto)
add_test(NAME browser-translate-service COMMAND dalinira-browser-translate-service-test)

add_executable(dalinira-browser-yt-dlp-fixture-v1 browser/native/tests/yt_dlp_version_fixture.cpp)
target_compile_definitions(dalinira-browser-yt-dlp-fixture-v1 PRIVATE DALINIRA_YTDLP_FIXTURE_VERSION="2099.01.01")
add_executable(dalinira-browser-yt-dlp-fixture-v2 browser/native/tests/yt_dlp_version_fixture.cpp)
target_compile_definitions(dalinira-browser-yt-dlp-fixture-v2 PRIVATE DALINIRA_YTDLP_FIXTURE_VERSION="2099.02.02")
add_executable(dalinira-browser-yt-dlp-update-manager-test
  browser/native/tests/yt_dlp_update_manager_test.cpp
  browser/native/downloads/yt_dlp_update_manager.cpp
  browser/native/downloads/media_download_service.cpp
  browser/native/core/adult_content_protection.cpp
  browser/native/core/security_utils.cpp)
target_link_libraries(dalinira-browser-yt-dlp-update-manager-test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME browser-yt-dlp-update-manager COMMAND dalinira-browser-yt-dlp-update-manager-test
  $<TARGET_FILE:dalinira-browser-yt-dlp-fixture-v1>
  $<TARGET_FILE:dalinira-browser-yt-dlp-fixture-v2>)
add_dependencies(dalinira-browser-yt-dlp-update-manager-test
  dalinira-browser-yt-dlp-fixture-v1 dalinira-browser-yt-dlp-fixture-v2)
set_tests_properties(browser-yt-dlp-update-manager PROPERTIES TIMEOUT 90)

add_executable(dalinira-browser-adaptive-audio-dsp-test
  browser/native/tests/adaptive_audio_dsp_test.cpp
  browser/native/audio/audio_platform_policy.cpp
  browser/native/audio/web_audio_effects_controller.cpp
  browser/native/audio/audio_effects_page.cpp
  browser/native/eq/eq_preset_repository.cpp
  browser/native/eq/eq_preset_page.cpp
  browser/native/desktop_tabs/tab_manager.cpp
  browser/native/desktop_tabs/tab_performance_manager.cpp
  browser/native/desktop_tabs/tab_lifecycle_adapter.cpp
  browser/native/desktop_tabs/tab_resource_model.cpp
  browser/native/engine/engine_view.cpp
  browser/native/desktop_tabs/system_memory_pressure_monitor.cpp)
target_link_libraries(dalinira-browser-adaptive-audio-dsp-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets Qt6::Svg)
target_compile_definitions(dalinira-browser-adaptive-audio-dsp-test PRIVATE
  DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH="dali/web-output-audiophile.generated.js"
  DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH="dali/web-equalizer-32.generated.js"
  DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH="dali/web-dynamic-compressor.generated.js"
  DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH="dali/web-limiter.generated.js"
  DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH="dali/web-bass-enhancer.generated.js"
  DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH="dali/web-auto-gain.generated.js")
add_dependencies(dalinira-browser-adaptive-audio-dsp-test dalinira-web-output-dali dalinira-eq-presets-runtime)
add_test(NAME browser-adaptive-audio-dsp COMMAND dalinira-browser-adaptive-audio-dsp-test)
set_tests_properties(browser-adaptive-audio-dsp PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")

add_executable(dalinira-browser-blocker-test
  ${I18N_SOURCES}
  browser/native/tests/dalinira_blocker_test.cpp
  ${BLOCKER_SOURCES}
  browser/native/newtab/new_tab_html.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/security_utils.cpp)
target_link_libraries(dalinira-browser-blocker-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Concurrent Qt6::Svg)
target_compile_definitions(dalinira-browser-blocker-test PRIVATE
  DALINIRA_BROWSER_VERSION="${PROJECT_VERSION}")
add_test(NAME browser-blocker COMMAND dalinira-browser-blocker-test)
set_tests_properties(browser-blocker PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 90)
add_test(NAME browser-blocker-chromium-picker
  COMMAND dalinira-browser-blocker-test --chromium-picker-only)
set_tests_properties(browser-blocker-chromium-picker PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 60)
add_test(NAME browser-blocker-bounce-storage
  COMMAND dalinira-browser-blocker-test --bounce-storage-only)
set_tests_properties(browser-blocker-bounce-storage PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 60)
add_test(NAME browser-blocker-bounce-storage-chromium
  COMMAND dalinira-browser-blocker-test --chromium-bounce-storage-only)
set_tests_properties(browser-blocker-bounce-storage-chromium PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 60)

# Remote filter-list updates against a loopback HTTP fixture (no Internet),
# including one offscreen Chromium page and the WebKit filter's socket server.
add_executable(dalinira-browser-blocker-filter-update-test
  ${I18N_SOURCES}
  browser/native/tests/blocker_filter_update_test.cpp
  browser/native/engine/webkit/webkit_network_filter.cpp
  ${BLOCKER_SOURCES}
  browser/native/newtab/new_tab_html.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/security_utils.cpp)
target_link_libraries(dalinira-browser-blocker-filter-update-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Concurrent Qt6::Svg)
target_compile_definitions(dalinira-browser-blocker-filter-update-test PRIVATE
  DALINIRA_BROWSER_VERSION="${PROJECT_VERSION}"
  DALINIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME browser-blocker-filter-update COMMAND dalinira-browser-blocker-filter-update-test)
set_tests_properties(browser-blocker-filter-update PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 180 LABELS "integration;webengine")

# Filter syntax families ($removeparam, $redirect, $badfilter, $method,
# response-phase rules) through the shared model and both engine adapters.
add_executable(dalinira-browser-blocker-filter-syntax-test
  ${I18N_SOURCES}
  browser/native/tests/blocker_filter_syntax_test.cpp
  browser/native/engine/webkit/webkit_network_filter.cpp
  ${BLOCKER_SOURCES}
  browser/native/newtab/new_tab_html.cpp
  browser/native/core/search_engine_definition.cpp
  browser/native/core/security_utils.cpp)
target_link_libraries(dalinira-browser-blocker-filter-syntax-test PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Concurrent Qt6::Svg)
target_compile_definitions(dalinira-browser-blocker-filter-syntax-test PRIVATE
  DALINIRA_BROWSER_VERSION="${PROJECT_VERSION}"
  DALINIRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME browser-blocker-filter-syntax COMMAND dalinira-browser-blocker-filter-syntax-test)
set_tests_properties(browser-blocker-filter-syntax PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 180 LABELS "integration;webengine")

# These two UI tests intentionally use the application source set because the
# Settings and profile objects coordinate multiple subsystem hooks.
add_executable(dalinira-browser-performance-settings-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/performance_settings_test.cpp)
target_link_libraries(dalinira-browser-performance-settings-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-performance-settings-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-performance-settings-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-performance-settings COMMAND dalinira-browser-performance-settings-test)
set_tests_properties(browser-performance-settings PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

add_executable(dalinira-browser-tab-audible-fullscreen-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/tab_audible_and_fullscreen_test.cpp)
target_link_libraries(dalinira-browser-tab-audible-fullscreen-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-tab-audible-fullscreen-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-tab-audible-fullscreen-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-tab-audible-fullscreen COMMAND dalinira-browser-tab-audible-fullscreen-test)
set_tests_properties(browser-tab-audible-fullscreen PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu" TIMEOUT 90)

if(DALINIRA_BUILD_WEBKIT)
  add_executable(dalinira-browser-webkit-backend-test
    ${I18N_SOURCES}
    browser/native/tests/webkit_backend_test.cpp
    ${ENGINE_SOURCES}
    ${BLOCKER_SOURCES}
    browser/native/core/adult_content_protection.cpp
    browser/native/core/security_utils.cpp
    browser/native/core/search_engine_definition.cpp
    browser/native/core/search_suggestion_service.cpp
    browser/native/core/address_input_resolver.cpp
    browser/native/newtab/new_tab_html.cpp
    browser/native/newtab/new_tab_asset_resolver.cpp
    browser/native/newtab/new_tab_scheme.cpp
    browser/native/core/browser_icons.cpp
  )
  target_link_libraries(dalinira-browser-webkit-backend-test PRIVATE
    Qt6::Concurrent
    Qt6::Core
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::Network
    Qt6::Svg
    ${WEBKIT_LIBRARIES}
    ${X11_LIBRARIES}
  )
  target_include_directories(dalinira-browser-webkit-backend-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/engine
    ${WEBKIT_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
  )
  target_compile_definitions(dalinira-browser-webkit-backend-test PRIVATE
    DALINIRA_BROWSER_VERSION="${PROJECT_VERSION}")
  add_dependencies(dalinira-browser-webkit-backend-test dalinira-webkit-extension)
  add_test(NAME browser-webkit-newtab-shortcuts COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-webkit-backend-test> --newtab-shortcuts)
  set_tests_properties(browser-webkit-newtab-shortcuts PROPERTIES LABELS "gui" TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 ENVIRONMENT "GDK_BACKEND=x11;QT_QPA_PLATFORM=xcb")
  add_test(NAME browser-webkit-backend COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-webkit-backend-test>)
  set_tests_properties(browser-webkit-backend PROPERTIES
    ENVIRONMENT "GDK_BACKEND=x11;QT_QPA_PLATFORM=xcb" TIMEOUT 150)
  add_test(NAME browser-webkit-site-data
    COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-webkit-backend-test> --site-data-only)
  set_tests_properties(browser-webkit-site-data PROPERTIES
    LABELS "gui;privacy" ENVIRONMENT "GDK_BACKEND=x11;QT_QPA_PLATFORM=xcb" TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

  # Engine-neutral logic behind the WebKit backend; no window, no engine process.
  add_executable(dalinira-browser-webkit-engine-logic-test
    ${I18N_SOURCES}
    browser/native/tests/webkit_engine_logic_test.cpp
    ${ENGINE_SOURCES}
    browser/native/core/adult_content_protection.cpp
    browser/native/core/security_utils.cpp
    browser/native/core/search_engine_definition.cpp
    browser/native/core/search_suggestion_service.cpp
    browser/native/core/address_input_resolver.cpp
    browser/native/newtab/new_tab_html.cpp
    browser/native/newtab/new_tab_asset_resolver.cpp
    browser/native/newtab/new_tab_scheme.cpp
    browser/native/core/browser_icons.cpp
  )
  target_link_libraries(dalinira-browser-webkit-engine-logic-test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Svg ${WEBKIT_LIBRARIES} ${X11_LIBRARIES})
  target_include_directories(dalinira-browser-webkit-engine-logic-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/engine
    ${WEBKIT_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS})
  add_test(NAME browser-webkit-engine-logic COMMAND dalinira-browser-webkit-engine-logic-test)
  set_tests_properties(browser-webkit-engine-logic PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

  add_executable(dalinira-browser-webkit-media-test
    ${I18N_SOURCES}
    browser/native/tests/webkit_media_test.cpp
    ${ENGINE_SOURCES}
    browser/native/core/adult_content_protection.cpp
    browser/native/core/security_utils.cpp
    browser/native/core/search_engine_definition.cpp
    browser/native/newtab/new_tab_html.cpp
    browser/native/newtab/new_tab_asset_resolver.cpp
    browser/native/newtab/new_tab_scheme.cpp
    browser/native/core/browser_icons.cpp
  )
  target_link_libraries(dalinira-browser-webkit-media-test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Svg ${WEBKIT_LIBRARIES} ${X11_LIBRARIES})
  target_include_directories(dalinira-browser-webkit-media-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/engine
    ${WEBKIT_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS})
  target_compile_definitions(dalinira-browser-webkit-media-test PRIVATE
    DALINIRA_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/browser/native/tests/fixtures/media")
  add_test(NAME browser-webkit-media COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-webkit-media-test>)
  # ~35 s locally (includes WebKit's 10 s service worker delays); --live adds
  # YouTube start, a possible pre-roll ad and a seek.
  set_tests_properties(browser-webkit-media PROPERTIES
    ENVIRONMENT "GDK_BACKEND=x11;QT_QPA_PLATFORM=xcb" TIMEOUT 240)
endif()

# Tests that open real windows on the desktop display (GUI tier). They run
# through ${DALINIRA_GUI_GATE}, so a plain `ctest` skips them; run them with
# scripts/safe-gui-test.sh (one at a time, resource monitor, crash/GPU-error
# checks, no core dumps). Chromium GUI tests keep the GPU disabled, as the
# offscreen WebEngine tests do; WebKit tests use the desktop's GPU selection.
# Window-manager stress, fullscreen, live sites and memory profiling are a
# further opt-in tier (safe-gui-test.sh --manual / --live / --memory-profile).
foreach(DALINIRA_GUI_TEST browser-critical-tab-drag-scenario browser-runtime-feature-integration browser-phase2a-policy)
  set_tests_properties(${DALINIRA_GUI_TEST} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")
endforeach()
set(DALINIRA_GUI_TESTS
  browser-desktop-tab-system browser-tab-drag-controller browser-critical-tab-drag-scenario
  browser-runtime-feature-integration browser-phase2a-policy browser-step5-essential-features
  browser-tab-memory-ux browser-engine-workspace-switch browser-web-audio-engine-gui
  browser-web-audio-activation-latency browser-window-platform-xcb browser-window-platform-wayland
  browser-secure-dns-configuration browser-engine-parity-layout-wayland)
# Every parity case goes through the GUI gate; list them all so a plain or
# CI `ctest -LE gui` run skips them instead of reporting the gate as a failure.
foreach(DALINIRA_PARITY_CASE IN LISTS DALINIRA_PARITY_CASES)
  list(APPEND DALINIRA_GUI_TESTS browser-engine-parity-${DALINIRA_PARITY_CASE})
endforeach()
if(DALINIRA_BUILD_WEBKIT)
  list(APPEND DALINIRA_GUI_TESTS browser-webkit-backend browser-webkit-media browser-webkit-lifecycle)
endif()
set_tests_properties(${DALINIRA_GUI_TESTS} PROPERTIES RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

# X11 map check used by scripts/safe-gui-test.sh before any GUI test.
pkg_check_modules(DALINIRA_XLIB x11)
if(DALINIRA_XLIB_FOUND)
  add_executable(dalinira-x11-map-probe browser/native/tools/x11_map_probe.cpp)
  target_include_directories(dalinira-x11-map-probe PRIVATE ${DALINIRA_XLIB_INCLUDE_DIRS})
  set_property(TARGET dalinira-x11-map-probe PROPERTY LINK_LIBRARIES ${DALINIRA_XLIB_LIBRARIES})
  set_target_properties(dalinira-x11-map-probe PROPERTIES AUTOMOC OFF)
endif()

# Keep assertions enabled in Release and apply the same conservative warning
# coverage used by the application to all DaliNira-owned test executables.
get_property(DALINIRA_ALL_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(DALINIRA_TEST_TARGET IN LISTS DALINIRA_ALL_TEST_TARGETS)
  if(DALINIRA_TEST_TARGET MATCHES "[-_]test$")
    target_compile_options(${DALINIRA_TEST_TARGET} PRIVATE -UNDEBUG)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${DALINIRA_TEST_TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
  endif()
endforeach()

set_tests_properties(
  browser-credential-vault-test browser-password-autofill-test
  browser-password-manager-page-test browser-security-utils
  PROPERTIES LABELS "security;passwords")
set_tests_properties(
  browser-web-audio-effects browser-adaptive-audio-dsp
  PROPERTIES LABELS "audio;webengine")
set_tests_properties(
  browser-runtime-feature-integration dalinira-phase22d-integration
  browser-site-permission browser-context-menu browser-step5-essential-features
  browser-page-translator browser-state-transfer browser-tab-audible-fullscreen
  PROPERTIES LABELS "integration;webengine")

# WebEngine's Linux namespace sandbox cannot start in common containerized CI
# runners. CTest already uses disposable profiles and disables GPU rendering;
# explicitly disable the Chromium sandbox for these isolated test processes.
set_property(TEST
  browser-web-audio-effects browser-password-autofill-test
  browser-runtime-feature-integration browser-phase2a-policy
  browser-tab-scheduler-retry browser-tab-hover-card smart_omnibox_phase2_1_test
  dalinira-phase22d dalinira-phase22d-integration browser-site-permission
  browser-context-menu browser-step5-essential-features browser-tab-throbber
  browser-tab-manager browser-internal-tab browser-tab-performance-manager
  browser-page-translator browser-state-transfer browser-adaptive-audio-dsp
  browser-blocker browser-performance-settings browser-tab-audible-fullscreen
  APPEND PROPERTY ENVIRONMENT "QTWEBENGINE_DISABLE_SANDBOX=1")
# Appended last: other blocks above set these tests' LABELS.
set_property(TEST ${DALINIRA_GUI_TESTS} APPEND PROPERTY LABELS gui)

# Localization: one shared catalog for both engines.
# Stand-alone tests whose sources now show catalog strings link the manager.
foreach(DALINIRA_I18N_TARGET
    dalinira-browser-eq-preset-repository-test dalinira-browser-web-audio-effects-test
    dalinira-browser-web-audio-engine-unit-test dalinira-browser-web-audio-engine-stress-test
    dalinira-browser-credential-vault-test dalinira-browser-song-finder-test
    dalinira-browser-password-autofill-test dalinira-browser-media-download-service-test
    dalinira-password-manager-page-test dalinira-browser-general-download-manager-test
    dalinira-browser-phase2a-policy-test dalinira-browser-address-input-resolver-test
    dalinira-browser-omnibox-navigation-integration-test dalinira-browser-smart-omnibox-phase2-test
    dalinira-browser-new-tab-background-store-test dalinira-browser-page-translator-test
    dalinira-browser-translate-service-test dalinira-browser-yt-dlp-update-manager-test
    dalinira-browser-adaptive-audio-dsp-test dalinira-browser-webkit-download-parity-test)
  target_sources(${DALINIRA_I18N_TARGET} PRIVATE ${I18N_SOURCES})
  target_link_libraries(${DALINIRA_I18N_TARGET} PRIVATE Qt6::Gui)
endforeach()

add_executable(dalinira-browser-localization-ui-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/localization_ui_test.cpp)
target_link_libraries(dalinira-browser-localization-ui-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-localization-ui-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
target_compile_options(dalinira-browser-localization-ui-test PRIVATE -UNDEBUG)
add_dependencies(dalinira-browser-localization-ui-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-localization-ui COMMAND dalinira-browser-localization-ui-test)
set_tests_properties(browser-localization-ui PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1"
  TIMEOUT 180)

# One controlled GUI validation per engine (gated like the other GUI tests;
# run through scripts/safe-gui-test.sh).
add_executable(dalinira-browser-localization-gui-test
  ${DALINIRA_BROWSER_FULL_SOURCES}
  browser/native/tests/localization_gui_test.cpp)
target_link_libraries(dalinira-browser-localization-gui-test PRIVATE ${DALINIRA_BROWSER_FULL_LIBS})
if(DALINIRA_BROWSER_FULL_DEFS)
  target_compile_definitions(dalinira-browser-localization-gui-test PRIVATE ${DALINIRA_BROWSER_FULL_DEFS})
endif()
add_dependencies(dalinira-browser-localization-gui-test dalinira-browser-policy dalinira-web-output-dali)
add_test(NAME browser-localization-gui
         COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-localization-gui-test>)
set_tests_properties(browser-localization-gui PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=xcb;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox"
  TIMEOUT 240 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS gui)

if(DALINIRA_BUILD_WEBKIT)
  add_test(NAME browser-blocker-provenance-webkit-gui
           COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-localization-gui-test> --webkit)
  set_tests_properties(browser-blocker-provenance-webkit-gui PROPERTIES
    ENVIRONMENT "GDK_BACKEND=x11;QT_QPA_PLATFORM=xcb;DALINIRA_GUI_MAX_WEB_PROCESSES=2"
    TIMEOUT 240 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS gui)
endif()

if(DALINIRA_BUILD_WEBKIT)
  add_executable(dalinira-browser-localization-webkit-gui-test
    ${I18N_SOURCES}
    browser/native/tests/localization_webkit_gui_test.cpp
    ${ENGINE_SOURCES}
    browser/native/core/adult_content_protection.cpp
    browser/native/core/security_utils.cpp
    browser/native/core/search_engine_definition.cpp
    browser/native/core/search_suggestion_service.cpp
    browser/native/core/address_input_resolver.cpp
    browser/native/newtab/new_tab_html.cpp
    browser/native/newtab/new_tab_asset_resolver.cpp
    browser/native/newtab/new_tab_scheme.cpp
    browser/native/core/browser_icons.cpp
  )
  target_link_libraries(dalinira-browser-localization-webkit-gui-test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::Svg ${WEBKIT_LIBRARIES} ${X11_LIBRARIES})
  target_include_directories(dalinira-browser-localization-webkit-gui-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native
    ${CMAKE_CURRENT_SOURCE_DIR}/browser/native/engine
    ${WEBKIT_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS})
  add_dependencies(dalinira-browser-localization-webkit-gui-test dalinira-webkit-extension)
  add_test(NAME browser-localization-webkit-gui
           COMMAND ${DALINIRA_GUI_GATE} $<TARGET_FILE:dalinira-browser-localization-webkit-gui-test>)
  set_tests_properties(browser-localization-webkit-gui PROPERTIES
    ENVIRONMENT "GDK_BACKEND=x11;QT_QPA_PLATFORM=xcb"
    TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS gui)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME i18n-audit
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/i18n_audit.py)
  add_test(NAME i18n-audit-selftest
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/i18n_audit_selftest.py)
endif()

# Legacy tests assert the Turkish source wording. Pin their UI language so the
# result does not depend on the machine locale; language tests manage it.
get_property(DALINIRA_ALL_TESTS DIRECTORY PROPERTY TESTS)
foreach(DALINIRA_TEST ${DALINIRA_ALL_TESTS})
  if(NOT DALINIRA_TEST MATCHES "^(language-manager-test|browser-localization-.*|i18n-audit.*)$")
    set_property(TEST ${DALINIRA_TEST} APPEND PROPERTY ENVIRONMENT "DALINIRA_UI_LANGUAGE=tr")
  endif()
endforeach()

# -------------------------------------------------------------
# Installation Rules
# -------------------------------------------------------------
include(GNUInstallDirs)

set(DALINIRA_RUNTIME_INSTALL_DIR "lib/dalinira-browser")

# A test-only system-WebKit build must never become an installation or package;
# this runs first in cmake_install.cmake, before anything is copied.
if(DALINIRA_SYSTEM_WEBKIT_TEST_BUILD)
  install(CODE [[message(FATAL_ERROR
    "This is a TEST-ONLY build against the system WebKitGTK "
    "(DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS=ON); it cannot be installed or packaged.")]])
endif()

install(TARGETS dalinira-browser
  RUNTIME DESTINATION ${DALINIRA_RUNTIME_INSTALL_DIR}
)

# The Web Audio engine is compiled from the checked-in .dl sources.  Keep the
# generated modules beside the installed executable: WebAudioEffectsController
# deliberately loads these trusted modules from disk before injecting them into
# supported media pages.  Omitting this directory made development builds work
# while distro/AUR installs silently had no DSP engine.
install(FILES
  ${DALI_WEB_OUTPUT_MODULE}
  ${DALI_WEB_EQ32_MODULE}
  ${DALI_WEB_COMPRESSOR_MODULE}
  ${DALI_WEB_LIMITER_MODULE}
  ${DALI_WEB_BASS_ENHANCER_MODULE}
  ${DALI_WEB_AUTO_GAIN_MODULE}
  DESTINATION ${DALINIRA_RUNTIME_INSTALL_DIR}/dali
)

# The policy follows the same executable-relative lookup used by main.cpp.
install(FILES ${DALI_POLICY_JSON}
  DESTINATION ${DALINIRA_RUNTIME_INSTALL_DIR}
)

install(FILES
  ${DALINIRA_ADULT_DOMAINS_SNAPSHOT}
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/privacy/adult_allowlist.txt
  DESTINATION ${DALINIRA_RUNTIME_INSTALL_DIR}/resources/privacy
)

install(DIRECTORY i18n
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dalinira-browser
)

# Runtime images and SVG controls served by dalinira://newtab.
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/assets/new-tab/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dalinira-browser/new-tab
)

if(TARGET dalinira-webkit-extension)
  install(TARGETS dalinira-webkit-extension
    LIBRARY DESTINATION ${DALINIRA_RUNTIME_INSTALL_DIR}/webkit-extensions
  )
endif()

# The private WebKitGTK runtime: the libraries, the helper processes and the
# injected bundle at the paths compiled into libwebkit2gtk, its translations,
# BUILD-INFO and the license / corresponding-source material. Not shipped:
# headers, pkg-config files, development symlinks, WebKitWebDriver, jsc.
if(DALINIRA_BUILD_WEBKIT AND DALINIRA_WEBKIT_RUNTIME_SDK)
  set(DALINIRA_WEBKIT_RUNTIME_INSTALL_DIR "${DALINIRA_RUNTIME_INSTALL_DIR}/webkit")
  set_target_properties(dalinira-browser dalinira-webkit-extension PROPERTIES
    INSTALL_RPATH "${DALINIRA_WEBKIT_RUNTIME_PREFIX}/lib")
  install(DIRECTORY "${DALINIRA_WEBKIT_RUNTIME_STAGE}/lib/"
    DESTINATION ${DALINIRA_WEBKIT_RUNTIME_INSTALL_DIR}/lib
    USE_SOURCE_PERMISSIONS
    FILES_MATCHING
      PATTERN "libwebkit2gtk-4.1.so.*"
      PATTERN "libjavascriptcoregtk-4.1.so.*"
      PATTERN "libwebkit2gtkinjectedbundle.so"
      PATTERN "pkgconfig" EXCLUDE
  )
  install(PROGRAMS
    "${DALINIRA_WEBKIT_RUNTIME_STAGE}/libexec/webkit2gtk-4.1/WebKitWebProcess"
    "${DALINIRA_WEBKIT_RUNTIME_STAGE}/libexec/webkit2gtk-4.1/WebKitNetworkProcess"
    "${DALINIRA_WEBKIT_RUNTIME_STAGE}/libexec/webkit2gtk-4.1/WebKitGPUProcess"
    DESTINATION ${DALINIRA_WEBKIT_RUNTIME_INSTALL_DIR}/libexec/webkit2gtk-4.1
  )
  install(DIRECTORY "${DALINIRA_WEBKIT_RUNTIME_STAGE}/share/locale"
    DESTINATION ${DALINIRA_WEBKIT_RUNTIME_INSTALL_DIR}/share
  )
  install(FILES "${DALINIRA_WEBKIT_RUNTIME_STAGE}/BUILD-INFO"
    DESTINATION ${DALINIRA_WEBKIT_RUNTIME_INSTALL_DIR}
  )
  install(DIRECTORY "${DALINIRA_WEBKIT_RUNTIME_LEGAL_DIR}/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/dalinira-browser/webkitgtk
  )
endif()

if(TARGET dalinira-webengine-process)
  install(TARGETS dalinira-webengine-process
    RUNTIME DESTINATION ${DALINIRA_RUNTIME_INSTALL_DIR}
  )
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/linux/dalinira-browser.in
  ${CMAKE_CURRENT_BINARY_DIR}/dalinira-browser-launcher
  @ONLY
)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/dalinira-browser-launcher
  DESTINATION bin
  RENAME dalinira-browser
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/linux/dalinira.desktop.in
  ${CMAKE_CURRENT_BINARY_DIR}/dalinira.desktop
  @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dalinira.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Application icons
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-16.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-24.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/24x24/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-32.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-48.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-64.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-128.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-256.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser-512.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps RENAME dalinira.png)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/browser/assets/icons/dalinira-browser.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps RENAME dalinira.png)

# AutoEQ presets & adblock resources
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/eq-presets
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dalinira-browser
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/browser/resources/adblock
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dalinira-browser/resources
)

# Native .deb / .rpm packages, with the runtime (GStreamer media plugins) as
# package dependencies.
include(${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/cpack.cmake)
