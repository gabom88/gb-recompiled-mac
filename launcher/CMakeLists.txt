# GB Recompiled Launcher — graphical front end that drives gbrecomp, CMake and
# Ninja for the user (add a ROM, recompile, play). Uses the same SDL2 +
# Dear ImGui stack the runtime vendors; no extra dependencies.
set(GBRL_IMGUI_DIR ${CMAKE_SOURCE_DIR}/runtime/vendor/imgui)

add_executable(gbrecomp-launcher
    main.cpp
    ${GBRL_IMGUI_DIR}/imgui.cpp
    ${GBRL_IMGUI_DIR}/imgui_draw.cpp
    ${GBRL_IMGUI_DIR}/imgui_tables.cpp
    ${GBRL_IMGUI_DIR}/imgui_widgets.cpp
    ${GBRL_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${GBRL_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(gbrecomp-launcher PRIVATE
    ${GBRL_IMGUI_DIR} ${GBRL_IMGUI_DIR}/backends)
target_compile_features(gbrecomp-launcher PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(gbrecomp-launcher PRIVATE SDL2::SDL2 Threads::Threads)
