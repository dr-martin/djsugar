# FetchYtDlp.cmake
#
# Downloads the official yt-dlp standalone binary at configure time and
# exposes its on-disk path as the cache variable ${YTDLP_BINARY_PATH}, so
# the install() step can ship it next to the `mixxx` executable.
#
# Why bundle yt-dlp at all: the YouTube/Spotify library features in this fork
# need a maintained YouTube extractor. Hand-rolling InnerTube against Google
# breaks within months because the per-client API keys, signing schemes and
# PoToken anti-bot guard rotate constantly. yt-dlp ships a self-contained
# PyInstaller binary (no Python on the target machine, no shared libraries,
# Unlicense / public-domain) precisely for embedding scenarios like this, so
# bundling it is the smallest possible change that gives users a YouTube tab
# that "just works" with no external setup.
#
# Pinned to a known-good release; bump YTDLP_VERSION + the per-platform
# SHA-256 from the matching SHA2-256SUMS asset on GitHub Releases:
#   https://github.com/yt-dlp/yt-dlp/releases
#
# Desktop (Win/macOS/Linux): bundles the standard PyInstaller binary.
#
# Android: bundles a static Python 3.11 musl/aarch64 binary + the yt-dlp
# zipimport package.  PyInstaller binaries cannot run under Bionic, so we
# build yt-dlp from the universal zip and invoke it via an embedded Python
# interpreter at runtime.  The assets are installed into the APK's
# lib/extraResources/ folder so the C++ layer can discover them at runtime.

set(
  YTDLP_VERSION
  "2026.07.04"
  CACHE STRING
  "Pinned yt-dlp release tag to bundle inside Mixxx"
)

# ---------------------------------------------------------------------------
# Platform-specific yt-dlp binary (PyInstaller) for desktop
# ---------------------------------------------------------------------------
if(WIN32)
  set(_ytdlp_asset "yt-dlp.exe")
  set(
    _ytdlp_sha256
    "52fe3c26dcf71fbdc85b528589020bb0b8e383155cfa81b64dd447bbe35e24b8"
  )
  set(_ytdlp_install_name "yt-dlp.exe")
elseif(APPLE)
  set(_ytdlp_asset "yt-dlp_macos")
  set(
    _ytdlp_sha256
    "498bd0dae17855c599d371d68ec5bafc439a9d8640e838be25c765a9792f261b"
  )
  set(_ytdlp_install_name "yt-dlp")
elseif(UNIX)
  if(ANDROID)
    # Android: skip the desktop downloader entirely.
    # The youtubedl-android AAR is handled below in the Android block.
  else()
    set(_ytdlp_asset "yt-dlp_linux")
    set(
      _ytdlp_sha256
      "6bbb3d314cde4febe36e5fa1d55462e29c974f63444e707871834f6d8cc210ae"
    )
    set(_ytdlp_install_name "yt-dlp")
  endif()
else()
  message(STATUS "yt-dlp bundling skipped: unsupported platform")
  set(
    YTDLP_BINARY_PATH
    ""
    CACHE FILEPATH
    "yt-dlp binary to install with Mixxx"
    FORCE
  )
  return()
endif()

# Desktop downloader: skip entirely on Android.
if(NOT ANDROID)
  set(_ytdlp_cache_dir "${CMAKE_BINARY_DIR}/_deps/yt-dlp-${YTDLP_VERSION}")
  set(_ytdlp_path "${_ytdlp_cache_dir}/${_ytdlp_install_name}")
  set(
    _ytdlp_url
    "https://github.com/yt-dlp/yt-dlp/releases/download/${YTDLP_VERSION}/${_ytdlp_asset}"
  )

  file(MAKE_DIRECTORY "${_ytdlp_cache_dir}")

  # If a previous configure already produced a verified copy, reuse it. This
  # keeps incremental configures cheap and lets users seed the cache offline.
  set(_need_download TRUE)
  if(EXISTS "${_ytdlp_path}")
    file(SHA256 "${_ytdlp_path}" _have_sha)
    if(_have_sha STREQUAL _ytdlp_sha256)
      set(_need_download FALSE)
      message(STATUS "Using cached yt-dlp ${YTDLP_VERSION} at ${_ytdlp_path}")
    else()
      message(
        STATUS
        "Cached yt-dlp at ${_ytdlp_path} has wrong SHA-256, re-downloading"
      )
      file(REMOVE "${_ytdlp_path}")
    endif()
  endif()

  if(_need_download)
    # Match the existing prebuilt-deps download retry pattern used in
    # CMakeLists.txt — GitHub release CDN occasionally returns 0-byte bodies
    # or 5xx, and a single failed configure is hard to debug.
    set(_max_attempts 3)
    set(_attempt 1)
    set(_ytdlp_ok FALSE)
    while(_attempt LESS_EQUAL _max_attempts AND NOT _ytdlp_ok)
      message(
        STATUS
        "Downloading yt-dlp ${YTDLP_VERSION} (${_ytdlp_asset}), attempt ${_attempt}/${_max_attempts}"
      )
      # Omit EXPECTED_HASH here: when the download fails (e.g. network blocked
      # in a Flatpak or offline CI runner), file(DOWNLOAD ... EXPECTED_HASH ...)
      # emits a CMake Error that marks the whole configure as failed even though
      # our fallback path is perfectly valid. Verify the hash ourselves after a
      # successful download so a _corrupted_ binary still fails loudly without
      # a network failure causing a hard configure error.
      file(
        DOWNLOAD "${_ytdlp_url}" "${_ytdlp_path}"
        SHOW_PROGRESS
        STATUS _dl_status
        TLS_VERIFY ON
      )
      list(GET _dl_status 0 _dl_code)
      list(GET _dl_status 1 _dl_msg)
      if(_dl_code EQUAL 0 AND EXISTS "${_ytdlp_path}")
        file(SIZE "${_ytdlp_path}" _dl_size)
        if(_dl_size GREATER 0)
          # Verify the hash so a CDN glitch that returns garbage doesn't sneak
          # into a shipped binary. A hash mismatch is an unexpected hard error
          # (download infrastructure broken), not a soft network-blocked case.
          file(SHA256 "${_ytdlp_path}" _actual_sha)
          if(_actual_sha STREQUAL _ytdlp_sha256)
            set(_ytdlp_ok TRUE)
          else()
            message(
              WARNING
              "yt-dlp download hash mismatch (expected ${_ytdlp_sha256}, got ${_actual_sha}), retrying"
            )
            file(REMOVE "${_ytdlp_path}")
          endif()
        else()
          message(STATUS "Downloaded yt-dlp is 0 bytes, retrying")
          file(REMOVE "${_ytdlp_path}")
        endif()
      else()
        message(STATUS "yt-dlp download failed: ${_dl_msg}")
        file(REMOVE "${_ytdlp_path}")
      endif()
      math(EXPR _attempt "${_attempt} + 1")
    endwhile()

    if(NOT _ytdlp_ok)
      message(
        WARNING
        "Failed to download yt-dlp ${YTDLP_VERSION} from ${_ytdlp_url} after "
        "${_max_attempts} attempts. The YouTube tab will fall back to a "
        "yt-dlp on the user's PATH or pointed at by MIXXX_YTDLP. To bundle a "
        "binary, place ${_ytdlp_asset} at ${_ytdlp_path} (or fix network "
        "access) and re-run cmake."
      )
      set(
        YTDLP_BINARY_PATH
        ""
        CACHE FILEPATH
        "yt-dlp binary to install with Mixxx"
        FORCE
      )
      return()
    endif()
  endif()

  # Make the Linux / macOS binary executable. Windows .exe needs no chmod.
  if(NOT WIN32)
    file(
      CHMOD
      "${_ytdlp_path}"
      PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        OWNER_EXECUTE
        GROUP_READ
        GROUP_EXECUTE
        WORLD_READ
        WORLD_EXECUTE
    )
  endif()

  set(
    YTDLP_BINARY_PATH
    "${_ytdlp_path}"
    CACHE FILEPATH
    "yt-dlp binary to install with Mixxx"
    FORCE
  )
  message(
    STATUS
    "Mixxx will bundle yt-dlp ${YTDLP_VERSION} from ${YTDLP_BINARY_PATH}"
  )
endif()

# ---------------------------------------------------------------------------
# Android: youtubedl-android runtime
# ---------------------------------------------------------------------------
if(ANDROID)
  # packaging/android/build.gradle resolves the official Maven artifacts. Do
  # not manually unpack classes.jar: that drops the AAR resources, JNI runtime
  # and transitive dependencies that YoutubeDL.init()/execute() require.
  target_compile_definitions(mixxx-lib PUBLIC HAVE_YTDLP_ANDROID=1)
  message(
    STATUS
    "Android YouTube downloader will use youtubedl-android 0.18.1 via Gradle"
  )
endif()
