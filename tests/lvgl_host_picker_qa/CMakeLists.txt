cmake_minimum_required(VERSION 3.16)
project(friday_host_picker_qa LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
get_filename_component(QA_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
add_subdirectory("${QA_REPO_ROOT}/components/lvgl" lvgl-build)

add_executable(host_picker_qa
    main.cpp
    "${QA_REPO_ROOT}/main/apps/app_codex_micro/view/view.cpp"
    "${QA_REPO_ROOT}/main/assets/fonts/MontserratSemiBold26.c")
target_include_directories(host_picker_qa PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/stubs"
    "${QA_REPO_ROOT}/main")
target_link_libraries(host_picker_qa PRIVATE lvgl)

# The assertions exercise user input and must run in Release builds too.
target_compile_options(host_picker_qa PRIVATE -UNDEBUG)
