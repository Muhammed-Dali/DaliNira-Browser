if(NOT DEFINED DALINIRA_BUILD_DIR OR NOT DEFINED DALINIRA_INSTALL_BINDIR)
  message(FATAL_ERROR "DALINIRA_BUILD_DIR and DALINIRA_INSTALL_BINDIR are required")
endif()

set(stage "${DALINIRA_BUILD_DIR}/install-layout-test")
file(REMOVE_RECURSE "${stage}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${stage}"
          "${CMAKE_COMMAND}" --install "${DALINIRA_BUILD_DIR}" --prefix /usr
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "staged install failed:\n${install_output}\n${install_error}")
endif()

set(executable_root "${stage}/usr/lib/dalinira-browser")
set(required_files
  "${executable_root}/dalinira-browser"
  "${stage}/usr/bin/dalinira-browser"
  "${executable_root}/browser_policy.json"
  "${executable_root}/resources/privacy/adult_domains.bin"
  "${executable_root}/resources/privacy/adult_allowlist.txt"
  "${executable_root}/dali/web-output-audiophile.generated.js"
  "${executable_root}/dali/web-equalizer-32.generated.js"
  "${executable_root}/dali/web-dynamic-compressor.generated.js"
  "${executable_root}/dali/web-limiter.generated.js"
  "${executable_root}/dali/web-bass-enhancer.generated.js"
  "${executable_root}/dali/web-auto-gain.generated.js"
  "${stage}/usr/share/dalinira-browser/new-tab/dalinira-browser.png"
  "${stage}/usr/share/dalinira-browser/new-tab/icons/search.svg"
  "${stage}/usr/share/dalinira-browser/eq-presets/dalinira_presets.json"
  "${stage}/usr/share/applications/dalinira.desktop"
  "${stage}/usr/share/icons/hicolor/256x256/apps/dalinira.png"
  "${stage}/usr/share/icons/hicolor/128x128/apps/dalinira.png"
  "${stage}/usr/share/icons/hicolor/32x32/apps/dalinira.png"
  "${executable_root}/dalinira-webengine-process"
  "${stage}/usr/share/dalinira-browser/i18n"
  "${stage}/usr/share/dalinira-browser/resources/adblock/NOTICE.txt"
)
# The WebKit engine's web process extension (network filtering), when built.
if(DALINIRA_EXPECT_WEBKIT)
  list(APPEND required_files "${executable_root}/webkit-extensions/libdalinira-webkit-extension.so")
endif()
# The private WebKitGTK runtime, when the build uses one: exactly the runtime
# part of WebKitGTK at the prefix compiled into it, plus its legal files.
set(webkit_runtime "${executable_root}/webkit")
if(DALINIRA_EXPECT_WEBKIT_RUNTIME)
  list(APPEND required_files
    "${webkit_runtime}/lib/libwebkit2gtk-4.1.so.0"
    "${webkit_runtime}/lib/libjavascriptcoregtk-4.1.so.0"
    "${webkit_runtime}/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so"
    "${webkit_runtime}/libexec/webkit2gtk-4.1/WebKitWebProcess"
    "${webkit_runtime}/libexec/webkit2gtk-4.1/WebKitNetworkProcess"
    "${webkit_runtime}/libexec/webkit2gtk-4.1/WebKitGPUProcess"
    "${webkit_runtime}/share/locale/tr/LC_MESSAGES/WebKitGTK-4.1.mo"
    "${webkit_runtime}/BUILD-INFO"
    "${stage}/usr/share/doc/dalinira-browser/webkitgtk/LICENSE.WebKitGTK"
    "${stage}/usr/share/doc/dalinira-browser/webkitgtk/SOURCE.md"
    "${stage}/usr/share/doc/dalinira-browser/webkitgtk/COPYRIGHT.debian"
    "${stage}/usr/share/doc/dalinira-browser/webkitgtk/BUILD-INFO"
  )
  file(GLOB runtime_patches "${stage}/usr/share/doc/dalinira-browser/webkitgtk/patches/*.patch")
  if(NOT runtime_patches)
    message(FATAL_ERROR "the WebKit runtime's patches are not installed with its license files")
  endif()
  foreach(development_file
      "${webkit_runtime}/include"
      "${webkit_runtime}/bin"
      "${webkit_runtime}/lib/pkgconfig"
      "${webkit_runtime}/lib/libwebkit2gtk-4.1.so"
      "${webkit_runtime}/lib/libjavascriptcoregtk-4.1.so"
      "${webkit_runtime}/libexec/webkit2gtk-4.1/jsc")
    if(EXISTS "${development_file}")
      message(FATAL_ERROR "development file in the packaged WebKit runtime: ${development_file}")
    endif()
  endforeach()
elseif(EXISTS "${webkit_runtime}")
  message(FATAL_ERROR "WebKit runtime installed by a build without one: ${webkit_runtime}")
endif()

foreach(required_file IN LISTS required_files)
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "installed runtime asset missing: ${required_file}")
  endif()
  if(IS_DIRECTORY "${required_file}")
    file(GLOB directory_entries "${required_file}/*")
    if(NOT directory_entries)
      message(FATAL_ERROR "installed runtime directory is empty: ${required_file}")
    endif()
    continue()
  endif()
  file(SIZE "${required_file}" required_size)
  if(required_size EQUAL 0)
    message(FATAL_ERROR "installed runtime asset is empty: ${required_file}")
  endif()
endforeach()

file(READ "${stage}/usr/share/applications/dalinira.desktop" desktop_entry)
foreach(expected_line
    "Name=DaliNira"
    "Exec=/usr/bin/dalinira-browser %U"
    "TryExec=/usr/bin/dalinira-browser"
    "Icon=dalinira"
    "StartupWMClass=DaliNiraBrowser"
    "StartupNotify=true"
    "MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;")
  string(FIND "${desktop_entry}" "${expected_line}" line_position)
  if(line_position EQUAL -1)
    message(FATAL_ERROR "desktop integration mismatch; missing: ${expected_line}")
  endif()
endforeach()

file(GLOB autoeq_files "${stage}/usr/share/dalinira-browser/eq-presets/autoeq/*.json")
list(LENGTH autoeq_files autoeq_count)
if(autoeq_count LESS 1750)
  message(FATAL_ERROR "installed AutoEQ collection is incomplete: ${autoeq_count}")
endif()

file(REMOVE_RECURSE "${stage}")
