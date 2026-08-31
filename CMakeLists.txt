cmake_minimum_required(VERSION 3.21)

# The project name is the INTERNAL identifier and is deliberately not the mod's public name.
# It fixes ShoutMCO.dll, ShoutMCO.ini and the SKSE log filename, and the capture tooling resolves
# ShoutMCO.log by that name. The public name lives on the MO2 folder, the readme and the Nexus
# page, which is where a stranger reads it.
project(
	ShoutMCO
	# Bumped the moment shipped src/ moves past the last release rather than at the next one. A
	# window where two different binaries both called themselves 1.0.0 is what that rule cost.
	VERSION 1.0.4
	DESCRIPTION "Shouts for MCO"
	LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

option(BUILD_SKYRIM "Build for Skyrim" OFF)
message(STATUS "BUILD_SKYRIM=${BUILD_SKYRIM}")
message(STATUS "ENABLE_SKYRIM_SE=${ENABLE_SKYRIM_SE}")
message(STATUS "ENABLE_SKYRIM_AE=${ENABLE_SKYRIM_AE}")
message(STATUS "ENABLE_SKYRIM_VR=${ENABLE_SKYRIM_VR}")

if(NOT BUILD_SKYRIM)
	message(FATAL_ERROR "Configure with the ALL preset so BUILD_SKYRIM=ON.")
endif()

# Spec targets SE 1.5.97 and AE 1.6.1170. VR is out of scope.
if(NOT ENABLE_SKYRIM_SE OR NOT ENABLE_SKYRIM_AE OR ENABLE_SKYRIM_VR)
	message(FATAL_ERROR "Use preset ALL (SE=ON, AE=ON, VR=OFF).")
endif()

add_compile_definitions(SKYRIM)
set(CommonLibPath "extern/CommonLibSSE-NG")
set(CommonLibName "CommonLibSSE")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_library("${PROJECT_NAME}" SHARED)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(SHOUTMCO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB SHOUTMCO_HEADERS CONFIGURE_DEPENDS
	"${SHOUTMCO_SRC_DIR}/*.h"
	"${SHOUTMCO_SRC_DIR}/*.hpp"
)
file(GLOB SHOUTMCO_SOURCES CONFIGURE_DEPENDS
	"${SHOUTMCO_SRC_DIR}/*.cpp"
)

source_group(TREE "${SHOUTMCO_SRC_DIR}" PREFIX "Header Files" FILES ${SHOUTMCO_HEADERS})
source_group(TREE "${SHOUTMCO_SRC_DIR}" PREFIX "Source Files" FILES ${SHOUTMCO_SOURCES})

target_sources("${PROJECT_NAME}" PRIVATE ${SHOUTMCO_HEADERS} ${SHOUTMCO_SOURCES})

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
	"${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc"
	@ONLY
)
target_sources("${PROJECT_NAME}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc")

target_precompile_headers(
	"${PROJECT_NAME}"
	PRIVATE
	"${SHOUTMCO_SRC_DIR}/PCH.h"
)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
	add_compile_definitions(_UNICODE)
	target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:DEBUG>")

	target_compile_options(
		"${PROJECT_NAME}"
		PRIVATE
		/MP
		/W4
		/permissive-
		/Zc:preprocessor
		/Zc:__cplusplus
		/EHsc
		/wd4200
	)

	target_compile_options(${PROJECT_NAME} PUBLIC "$<$<CONFIG:DEBUG>:/Od;/Zi>")
	target_compile_options(${PROJECT_NAME} PUBLIC "$<$<CONFIG:RELEASE>:/O2;/Zi>")

	target_link_options(
		${PROJECT_NAME}
		PRIVATE
		"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif()

add_subdirectory(${CommonLibPath} ${CommonLibName} EXCLUDE_FROM_ALL)

include("${CMAKE_CURRENT_SOURCE_DIR}/${CommonLibPath}/cmake/CommonLibSSE.cmake")
# NAME is what SKSE reports and what names the log file -- ShoutMCO.log. It must not drift from
# PROJECT_NAME above, or capture.ps1 and every archived trace path stop resolving.
#
# AUTHOR previously held the mod's name, which put the wrong string in the field SKSE surfaces as
# the author.
target_commonlibsse_properties(
	"${PROJECT_NAME}"
	NAME "ShoutMCO"
	AUTHOR "Leitmotives"
	EMAIL ""
)

find_package(spdlog CONFIG REQUIRED)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${SHOUTMCO_SRC_DIR}
	# The SHIPPED public headers. Compiled here as well as by any consumer, so the contract
	# cannot drift from the implementation that serves it.
	${CMAKE_CURRENT_SOURCE_DIR}/include
	# Vendored third-party headers. SKSEMenuFramework.h is the menu host's own consumer header:
	# it resolves the framework through GetProcAddress at runtime, so there is nothing to link
	# and a missing framework is a null function pointer rather than a load failure.
	${CMAKE_CURRENT_SOURCE_DIR}/extern
	${CMAKE_CURRENT_BINARY_DIR}/cmake
)

target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)

# Stage to dist/ only. Deployment into the MO2 mods folder is a deliberate, separate
# step -- see AGENTS.md: never write into STOCK GAME\Data, and preview any deployment.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${PROJECT_NAME}>"
		"${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/ShoutMCO.dll"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_PDB_FILE:${PROJECT_NAME}>"
		"${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/ShoutMCO.pdb"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/config/ShoutMCO.ini"
		"${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/ShoutMCO.ini"
	# The header ships alongside the DLL, so a driver author has the contract without needing
	# this repository.
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist/include"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/include/ShoutMCO_CastIntent.h"
		"${CMAKE_SOURCE_DIR}/dist/include/ShoutMCO_CastIntent.h"
	COMMENT "Staging ShoutMCO to dist/SKSE/Plugins"
)
