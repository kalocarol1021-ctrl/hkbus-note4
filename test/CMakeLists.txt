# Host (PC) build of the display/ETA core for tests and screen previews.
# Needs a cJSON source tree: by default the copy inside ESP-IDF.
cmake_minimum_required(VERSION 3.16)
project(hkbus_host CXX C)
set(CMAKE_CXX_STANDARD 17)

if(NOT CJSON_DIR)
  set(CJSON_DIR "$ENV{IDF_PATH}/components/json/cJSON")
endif()
set(CORE ${CMAKE_CURRENT_LIST_DIR}/../components/hkbus_core)

add_library(hkcore STATIC
  ${CORE}/hk_fb.cc ${CORE}/hk_font.cc ${CORE}/hk_model.cc ${CORE}/hk_screen.cc ${CORE}/hk_weather.cc
  ${CORE}/third_party/qrcodegen.c ${CJSON_DIR}/cJSON.c)
target_include_directories(hkcore PUBLIC ${CORE}/include ${CORE}/third_party ${CJSON_DIR})
target_compile_options(hkcore PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(hk_host_test host_test.cc)
target_link_libraries(hk_host_test hkcore)
add_executable(hk_preview host_preview.cc)
target_link_libraries(hk_preview hkcore)
