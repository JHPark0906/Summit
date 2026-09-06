file(GLOB_RECURSE SUMMIT_COMPONENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

set(SUMMIT_BOOTSTRAP_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Bootstrap/SummitBootstrap.cpp")
# Components are shared with GameEditor, which registers its own bootstrap.
list(REMOVE_ITEM SUMMIT_COMPONENT_SOURCES "${SUMMIT_BOOTSTRAP_SOURCE}")

if(NOT COMMAND gameengine_add_game_project)
    message(FATAL_ERROR
        "Summit must be configured through the GameEngine build. "
        "Set GAMEEDITOR_PROJECT_DIRECTORY to this project directory.")
endif()

gameengine_add_game_project(Summit CONTENT_DIR "Content" SOURCES ${SUMMIT_COMPONENT_SOURCES})

# The standalone player owns this bootstrap; the editor has its own bootstrap.
target_sources(Summit PRIVATE "${SUMMIT_BOOTSTRAP_SOURCE}")

set(SUMMIT_SERVERCORE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../ServerCore"
    CACHE PATH "Path to the ServerCore source tree used for its header-only frame and datagram codecs")

if(NOT EXISTS "${SUMMIT_SERVERCORE_ROOT}/include/ServerCore/Protocol/FrameCodec.h" OR
   NOT EXISTS "${SUMMIT_SERVERCORE_ROOT}/include/ServerCore/Protocol/DatagramCodec.h")
    message(FATAL_ERROR
        "Summit requires ServerCore's FrameCodec.h and DatagramCodec.h. "
        "Set SUMMIT_SERVERCORE_ROOT to the ServerCore source directory.")
endif()

target_include_directories(Summit_Components PRIVATE
    "${SUMMIT_SERVERCORE_ROOT}/include")

if(WIN32)
    target_link_libraries(Summit_Components PUBLIC ws2_32)
else()
    message(FATAL_ERROR "Summit's minimal multiplayer client currently supports Windows only.")
endif()

# These regressions need the local game project, production scene and Korean font.
# The regressions themselves validate the scene's asset references and content details.
# Source-only checkouts can build the player and components without registering them.
set(SUMMIT_TEST_REQUIRED_CONTENT
    "Summit.gameproject"
    "Scenes/InGame.scene"
    "Fonts/NanumSquareNeoOTF-Bd.otf")
set(SUMMIT_TEST_MISSING_CONTENT)
foreach(relative_path IN LISTS SUMMIT_TEST_REQUIRED_CONTENT)
    set(content_path "${CMAKE_CURRENT_SOURCE_DIR}/Content/${relative_path}")
    if(NOT EXISTS "${content_path}" OR IS_DIRECTORY "${content_path}")
        list(APPEND SUMMIT_TEST_MISSING_CONTENT "Content/${relative_path}")
    endif()
endforeach()
set(SUMMIT_TEST_CONTENT_AVAILABLE ON)
if(SUMMIT_TEST_MISSING_CONTENT)
    set(SUMMIT_TEST_CONTENT_AVAILABLE OFF)
endif()

option(SUMMIT_BUILD_TESTS "Build Summit gameplay and UI regression tests (requires local Content)"
    ${SUMMIT_TEST_CONTENT_AVAILABLE})
if(SUMMIT_BUILD_TESTS AND NOT SUMMIT_TEST_CONTENT_AVAILABLE)
    list(JOIN SUMMIT_TEST_MISSING_CONTENT "\n  " missing_content)
    message(FATAL_ERROR
        "SUMMIT_BUILD_TESTS=ON requires Summit's local regression content.\n"
        "Missing files:\n  ${missing_content}\n"
        "Restore the complete Content directory, including its .meta files, "
        "or configure with -DSUMMIT_BUILD_TESTS=OFF to build only the code.")
endif()
if(NOT SUMMIT_BUILD_TESTS AND NOT SUMMIT_TEST_CONTENT_AVAILABLE)
    message(STATUS "Summit: required local regression inputs are missing; gameplay and UI tests are disabled.")
endif()
if(SUMMIT_BUILD_TESTS)
    enable_testing()
    add_executable(SummitTests
        Tests/CharacterProfileTests.cpp
        Tests/ChatUiTests.cpp
        Tests/ChatNetworkTests.cpp
        Tests/SpeechBubbleTests.cpp
        Tests/RespawnTests.cpp
        Tests/OneWayPlatformTests.cpp
        Tests/GameplayAudioTests.cpp
        Tests/ChatVisualTests.cpp
        Tests/CameraBackgroundTests.cpp)
    gameengine_apply_common_options(SummitTests)
    target_link_libraries(SummitTests PRIVATE Summit_Components GameEngine)
    target_include_directories(SummitTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(SummitTests PRIVATE
        SUMMIT_TEST_CONTENT_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/Content"
        SUMMIT_TEST_CAPTURE_DIRECTORY="${CMAKE_BINARY_DIR}/../chat-visual-captures")
    gameengine_stage_runtime_files(SummitTests)
    add_test(NAME Summit.CharacterProfiles COMMAND SummitTests)
    set_tests_properties(Summit.CharacterProfiles PROPERTIES TIMEOUT 90)
    add_test(NAME Summit.ChatVisuals COMMAND SummitTests --chat-visual)
    set_tests_properties(Summit.ChatVisuals PROPERTIES TIMEOUT 180)
endif()
