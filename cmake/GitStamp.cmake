# Stamp the git commit that produced this binary into its version resource.
#
# WHY THIS EXISTS. The version resource already ties the DLL to CMakeLists' VERSION, and
# tools/package.ps1 already writes the packaging commit into the shipped README and the deployed
# meta.ini. Nothing tied the two together, so a DLL built from one tree could be packaged and
# stamped with another tree's commit -- which is exactly what happened: the deployed
# `MODS\mods\Thu'um Reborn` carried the published 1.0 binary from the public repo under a meta.ini
# naming a private-repo commit whose source cannot have produced it (ticket 80). Every log read off
# that install was ambiguous about which binary wrote it.
#
# The stamp goes in the binary at BUILD time, and package.ps1 reads it back and refuses a mismatch.
# A wrong claim is now a thrown error rather than a line somebody has to check by hand.
#
# Run two ways, deliberately:
#   include()  at configure time, so version.rc exists for target_sources
#   cmake -P   before every build, so the stamp tracks HEAD rather than the last configure
#
# Expects: SHOUTMCO_REPO_DIR, SHOUTMCO_VERSION_RC_IN, SHOUTMCO_VERSION_RC,
#          PROJECT_NAME, PROJECT_VERSION, PROJECT_VERSION_MAJOR/MINOR/PATCH

find_package(Git QUIET)

set(SHOUTMCO_GIT_COMMIT "unknown")
if(GIT_EXECUTABLE)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${SHOUTMCO_REPO_DIR}" rev-parse --short=12 HEAD
		OUTPUT_VARIABLE _shoutmco_sha
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE _shoutmco_rc
		ERROR_QUIET
	)
	if(_shoutmco_rc EQUAL 0 AND _shoutmco_sha)
		set(SHOUTMCO_GIT_COMMIT "${_shoutmco_sha}")
		# A dirty tree is stamped as such rather than refused. Refusing here would block every
		# ordinary development build; package.ps1 is where a dirty artifact is gated, and it
		# already has -AllowDirty for the deliberate case.
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${SHOUTMCO_REPO_DIR}" status --porcelain
			OUTPUT_VARIABLE _shoutmco_dirty
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
		if(_shoutmco_dirty)
			set(SHOUTMCO_GIT_COMMIT "${SHOUTMCO_GIT_COMMIT}+dirty")
		endif()
	endif()
endif()

# Written through a temporary and copied only when it changed. Rewriting version.rc unconditionally
# would relink the DLL on every build, which is a real cost on a target that takes minutes.
configure_file("${SHOUTMCO_VERSION_RC_IN}" "${SHOUTMCO_VERSION_RC}.tmp" @ONLY)
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SHOUTMCO_VERSION_RC}.tmp" "${SHOUTMCO_VERSION_RC}"
)
