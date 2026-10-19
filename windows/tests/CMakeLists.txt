cmake_minimum_required(VERSION 3.25)

# Also usable without the TSF DLL, the Windows SDK or the engine.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MsimeTsfLocalPolicyTests LANGUAGES CXX)
    enable_testing()
    # windows/CMakeLists.txt sets this for the in-tree build; the sources and headers are UTF-8.
    if(MSVC)
        add_compile_options(/utf-8)
    endif()
endif()

add_executable(test_vim_mode vim_mode.cpp ../src/Key/VimMode.cpp)
target_include_directories(test_vim_mode PRIVATE ../src)
target_compile_features(test_vim_mode PRIVATE cxx_std_17)
add_test(NAME vim_mode COMMAND test_vim_mode)

# Shared final voice-delivery wire contract. This target has no SDK, COM or pipe
# dependency; it verifies the binary codec and optional negotiation bit offline.
add_executable(test_voice_final_contract voice_final_contract.cpp)
target_include_directories(test_voice_final_contract PRIVATE ../../engine/contracts)
target_compile_features(test_voice_final_contract PRIVATE cxx_std_17)
add_test(NAME voice_final_contract COMMAND test_voice_final_contract)

add_executable(test_client_fallback client_fallback.cpp)
target_include_directories(test_client_fallback PRIVATE ../src)
target_compile_features(test_client_fallback PRIVATE cxx_std_17)
add_test(NAME client_fallback COMMAND test_client_fallback)

add_executable(test_key_repeat_guard key_repeat_guard.cpp)
target_include_directories(test_key_repeat_guard PRIVATE ../src)
target_compile_features(test_key_repeat_guard PRIVATE cxx_std_17)
add_test(NAME key_repeat_guard COMMAND test_key_repeat_guard)

add_executable(test_caps_lock_punctuation_policy caps_lock_punctuation_policy.cpp)
target_include_directories(test_caps_lock_punctuation_policy PRIVATE ../src)
target_compile_features(test_caps_lock_punctuation_policy PRIVATE cxx_std_17)
add_test(NAME caps_lock_punctuation_policy COMMAND test_caps_lock_punctuation_policy)

add_executable(test_caps_lock_input_policy caps_lock_input_policy.cpp)
target_include_directories(test_caps_lock_input_policy PRIVATE ../src)
target_compile_features(test_caps_lock_input_policy PRIVATE cxx_std_17)
add_test(NAME caps_lock_input_policy COMMAND test_caps_lock_input_policy)

add_executable(test_commit_candidate_and_continue_payload commit_candidate_and_continue_payload.cpp)
target_include_directories(test_commit_candidate_and_continue_payload PRIVATE ../src)
target_compile_features(test_commit_candidate_and_continue_payload PRIVATE cxx_std_17)
add_test(NAME commit_candidate_and_continue_payload COMMAND test_commit_candidate_and_continue_payload)

add_executable(test_punctuation_commit_text punctuation_commit_text.cpp)
target_include_directories(test_punctuation_commit_text PRIVATE ../src)
target_compile_features(test_punctuation_commit_text PRIVATE cxx_std_17)
add_test(NAME punctuation_commit_text COMMAND test_punctuation_commit_text)

add_executable(test_input_state_shortcuts input_state_shortcuts.cpp)
target_compile_features(test_input_state_shortcuts PRIVATE cxx_std_17)
target_compile_definitions(test_input_state_shortcuts PRIVATE MSIME_SHORTCUT_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../engine/contracts/tests/input_state_shortcuts.json")
add_test(NAME input_state_shortcuts COMMAND test_input_state_shortcuts)

# The classifier and the event queue are compiled into this target directly:
# they must stay free of TSF and DLL lifecycle dependencies.
add_executable(test_statistics statistics.cpp ../src/Statistics/char_classify.cpp)
target_include_directories(test_statistics PRIVATE ../src)
target_compile_features(test_statistics PRIVATE cxx_std_17)
add_test(NAME statistics COMMAND test_statistics)

add_executable(test_key_focus_recovery key_focus_recovery.cpp)
target_include_directories(test_key_focus_recovery PRIVATE ../src)
target_compile_features(test_key_focus_recovery PRIVATE cxx_std_17)
add_test(NAME key_focus_recovery COMMAND test_key_focus_recovery)

add_executable(test_deferred_key_failure_policy deferred_key_failure_policy.cpp)
target_include_directories(test_deferred_key_failure_policy PRIVATE ../src)
target_compile_features(test_deferred_key_failure_policy PRIVATE cxx_std_17)
add_test(NAME deferred_key_failure_policy COMMAND test_deferred_key_failure_policy)

add_executable(test_caret_anchor_policy caret_anchor_policy.cpp)
target_include_directories(test_caret_anchor_policy PRIVATE ../src)
target_compile_features(test_caret_anchor_policy PRIVATE cxx_std_17)
add_test(NAME caret_anchor_policy COMMAND test_caret_anchor_policy)

add_executable(test_focus_announcement_policy focus_announcement_policy.cpp)
target_include_directories(test_focus_announcement_policy PRIVATE ../src)
target_compile_features(test_focus_announcement_policy PRIVATE cxx_std_17)
add_test(NAME focus_announcement_policy COMMAND test_focus_announcement_policy)

# Exercise the production reconnect sequencing without loading a TIP into a host.
# Regenerate the fixture when either handler changes, so a missing recovery step
# cannot be hidden by a copied implementation in the test.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(reconnect_window_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/IME/MetasequoiaIME_WindowProc.cpp")
set(reconnect_key_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/Key/KeyEventSink_DeferredKeys.cpp")
set(reconnect_fixture_generator "${CMAKE_CURRENT_SOURCE_DIR}/generate_reconnect_fixture.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${reconnect_window_source}" "${reconnect_key_source}" "${reconnect_fixture_generator}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${reconnect_fixture_generator}"
        "${reconnect_window_source}" "${reconnect_key_source}"
        "${CMAKE_CURRENT_BINARY_DIR}/reconnect_recovery_source.h"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(test_reconnect_recovery reconnect_recovery.cpp)
target_include_directories(test_reconnect_recovery PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(test_reconnect_recovery PRIVATE cxx_std_17)
add_test(NAME reconnect_recovery COMMAND test_reconnect_recovery)

# Link the same inline key classifier used by normal and queued TSF paths.
if(WIN32)
    add_executable(test_temporary_japanese_input temporary_japanese_input.cpp)
    target_include_directories(test_temporary_japanese_input PRIVATE ../src/Header)
    target_compile_features(test_temporary_japanese_input PRIVATE cxx_std_17)
    add_test(NAME temporary_japanese_input COMMAND test_temporary_japanese_input)

    # Link the production strict edit session, then exercise its real TSF document calls through
    # full ITfContext/ITfRange/ITfComposition vtable mocks.
    add_executable(test_voice_final_edit_session
        voice_final_edit_session.cpp
        ../src/Edit/VoiceFinalEditSession.cpp)
    target_include_directories(test_voice_final_edit_session PRIVATE ../src)
    target_compile_features(test_voice_final_edit_session PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_final_edit_session PRIVATE UNICODE _UNICODE)
    target_link_libraries(test_voice_final_edit_session PRIVATE ole32 uuid)
    add_test(NAME voice_final_edit_session COMMAND test_voice_final_edit_session)

    # Dedicated preview execution: real production edit session plus full TSF vtable mocks,
    # including ITfContextComposition and ITfCompositionSink.
    add_executable(test_voice_preview_edit_session
        voice_preview_edit_session.cpp
        ../src/Edit/VoicePreviewEditSession.cpp
        ../src/Edit/VoiceFinalEditSession.cpp)
    target_include_directories(test_voice_preview_edit_session PRIVATE ../src)
    target_compile_features(test_voice_preview_edit_session PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_preview_edit_session PRIVATE UNICODE _UNICODE)
    target_link_libraries(test_voice_preview_edit_session PRIVATE ole32 uuid)
    add_test(NAME voice_preview_edit_session COMMAND test_voice_preview_edit_session)

    # Real message-mode named-pipe fixture. The production pump performs overlapped read and
    # write operations through the Windows SDK; the test observes actual ERROR_IO_PENDING.
    add_executable(test_worker_duplex_pump
        worker_duplex_pump.cpp
        ../src/IPC/WorkerDuplexPump.cpp)
    target_include_directories(test_worker_duplex_pump PRIVATE ../src)
    target_compile_features(test_worker_duplex_pump PRIVATE cxx_std_17)
    target_compile_definitions(test_worker_duplex_pump PRIVATE UNICODE _UNICODE)
    add_test(NAME worker_duplex_pump COMMAND test_worker_duplex_pump)
    set_tests_properties(worker_duplex_pump PROPERTIES TIMEOUT 20)

    add_executable(test_voice_delivery_registry voice_delivery_registry.cpp ../src/IPC/VoiceDeliveryRegistry.cpp)
    target_include_directories(test_voice_delivery_registry PRIVATE ../src)
    target_compile_features(test_voice_delivery_registry PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_delivery_registry PRIVATE UNICODE _UNICODE)
    add_test(NAME voice_delivery_registry COMMAND test_voice_delivery_registry)

    set(voice_reset_inputs
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/IME/MetasequoiaIME_VoiceDelivery.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/IME/MetasequoiaIME.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/Key/KeyHandler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/Composition/Composition.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/Composition/EndComposition.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_voice_reset_fixture.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${voice_reset_inputs})
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_voice_reset_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    add_executable(test_voice_reset_integration voice_reset_integration.cpp
        ../src/Edit/VoiceFinalEditSession.cpp ../src/Edit/VoicePreviewEditSession.cpp
        ../src/IPC/VoiceDeliveryRegistry.cpp)
    target_include_directories(test_voice_reset_integration PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_features(test_voice_reset_integration PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_reset_integration PRIVATE UNICODE _UNICODE)
    target_link_libraries(test_voice_reset_integration PRIVATE ole32 uuid user32)
    add_test(NAME voice_reset_integration COMMAND test_voice_reset_integration)
endif()

# Exercise the session-side commit-reply contract without loading a TIP into a host. The paths
# that decide what an unready committing reply costs are compiled from the production sources
# (see generate_commit_reply_fixture.py); only the pipe probe, the message window and the clock
# are doubles. Regenerate whenever one of those paths changes, so a removed step cannot hide
# behind a copy in the test.
set(commit_reply_ipc_header "${CMAKE_CURRENT_SOURCE_DIR}/../src/IPC/Ipc.h")
set(commit_reply_ipc_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/IPC/Ipc.cpp")
set(commit_reply_ime_header "${CMAKE_CURRENT_SOURCE_DIR}/../src/IME/MetasequoiaIME.h")
set(commit_reply_async_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/IME/MetasequoiaIME_AsyncRequests.cpp")
set(commit_reply_session_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/Key/KeyHandlerEditSession.cpp")
set(commit_reply_fixture_generator "${CMAKE_CURRENT_SOURCE_DIR}/generate_commit_reply_fixture.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${commit_reply_ipc_header}" "${commit_reply_ipc_source}" "${commit_reply_ime_header}"
    "${commit_reply_async_source}" "${commit_reply_session_source}" "${commit_reply_fixture_generator}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${commit_reply_fixture_generator}"
        "${commit_reply_ipc_header}" "${commit_reply_ipc_source}" "${commit_reply_ime_header}"
        "${commit_reply_async_source}" "${commit_reply_session_source}"
        "${CMAKE_CURRENT_BINARY_DIR}/commit_reply_types_source.h"
        "${CMAKE_CURRENT_BINARY_DIR}/commit_reply_source.h"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(test_commit_reply_not_ready commit_reply_not_ready.cpp)
target_include_directories(test_commit_reply_not_ready PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(test_commit_reply_not_ready PRIVATE cxx_std_17)
add_test(NAME commit_reply_not_ready COMMAND test_commit_reply_not_ready)
