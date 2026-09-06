# Host tests that must not pull in CommonLibSSE.
#
# A STANDALONE PROJECT, NOT A TARGET OF THE ROOT BUILD. The root CMakeLists hard-fails without
# BUILD_SKYRIM and pulls in CommonLibSSE, so a test target there would drag the whole game SDK in
# to compile a state machine that deliberately has no game in it. Configure this directory
# directly instead:
#
#   cmake -S tests -B build-tests
#   cmake --build build-tests --config Release
#   ctest --test-dir build-tests -C Release --output-on-failure
cmake_minimum_required(VERSION 3.21)

project(ShoutMCOTests VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

# One test starts a thread to prove the seam's guard is per-thread; harmless for the rest.
find_package(Threads REQUIRED)

function(shoutmco_host_test name source)
	add_executable(${name} ${source})
	target_compile_features(${name} PRIVATE cxx_std_23)
	target_include_directories(${name} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/../src"
		"${CMAKE_CURRENT_SOURCE_DIR}/../include"
	)
	if(MSVC)
		target_compile_options(${name} PRIVATE /W4 /permissive- /Zc:preprocessor /EHsc)
	else()
		target_compile_options(${name} PRIVATE -Wall -Wextra)
	endif()
	# The repository root, for the one test that reads shipped files rather than linking code.
	target_compile_definitions(${name} PRIVATE
		SHOUTMCO_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
	)
	target_link_libraries(${name} PRIVATE Threads::Threads)
	add_test(NAME ${name} COMMAND ${name})
endfunction()

# The slot header lives with the engine; the ABI header is the shipped public one. Both are
# compiled here exactly as the DLL compiles them -- a test against a copy would prove nothing.
shoutmco_host_test(cast_intent_tests cast_intent_tests.cpp)
# Fail-open / intercept routing for the clip-owned release, no game types.
shoutmco_host_test(clip_owned_release_tests clip_owned_release_tests.cpp)
# The attack-queued release waits for IsAttacking to fall, no game types.
shoutmco_host_test(attack_queued_release_tests attack_queued_release_tests.cpp)
# Tap-responsive shouts: the fShoutTime1 / fShoutTime2 override decision, no game types.
shoutmco_host_test(shout_hold_thresholds_tests shout_hold_thresholds_tests.cpp)
# The compiled defaults against the shipped INI. Reads the three source files as TEXT rather than
# linking Settings.h, which would drag CommonLibSSE into a host test -- see the test's own header.
shoutmco_host_test(settings_ini_parity_tests settings_ini_parity_tests.cpp)
# The tuning menu's INI writer: one value changed, every comment left alone. No game types.
shoutmco_host_test(ini_rewrite_tests ini_rewrite_tests.cpp)
# The power-attack seam: the outgoing-event match rule and its re-entrancy guard. No game types.
shoutmco_host_test(attack_seam_tests attack_seam_tests.cpp)
