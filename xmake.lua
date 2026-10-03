set_runtimes("MT")
add_rules("mode.debug", "mode.release")
set_languages("cxx20")

-- Both point at the checkouts under cs2kz-metamod by default; override either
-- with an environment variable to build against a different tree.
local SDK = os.getenv("HL2SDKCS2") or "C:/coding/cs2kz-metamod/hl2sdk-cs2"
local MMSOURCE = os.getenv("MMSOURCE") or "C:/coding/cs2kz-metamod/metamod-source"

-- plugin.cpp includes eiface.h for IVEngineServer2::ServerCommand, and eiface.h
-- includes network_connection.pb.h. That one generated header is the only
-- protobuf involved -- no type from it is ever used, so nothing has to be
-- compiled or linked -- and it is vendored in protobuf/ rather than generated,
-- which is what CS2ServerGUI does and what keeps the build self-contained.
-- protobuf/README.md has the regeneration command.
local PROTOBUF_HEADERS = os.getenv("CS2_PROTOBUF_HEADERS") or "protobuf"

target("cs2-panorama-debugger")
    set_kind("shared")
    set_symbols("debug", "embed")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")

    -- Only what ISmmPlugin.h, eiface.h and tier0's Msg() need. No mathlib,
    -- entity2 or tier1 translation units: the whole plugin is one library.
    add_includedirs(
        MMSOURCE .. "/core",
        MMSOURCE .. "/third_party/khook/include",
        PROTOBUF_HEADERS,
        SDK .. "/thirdparty/protobuf-3.21.8/src",
        SDK,
        SDK .. "/common",
        SDK .. "/game/shared",
        SDK .. "/game/server",
        SDK .. "/public",
        SDK .. "/public/engine",
        SDK .. "/public/mathlib",
        SDK .. "/public/tier0",
        SDK .. "/public/tier1",
        SDK .. "/public/entity2")

    add_defines(
        "COMPILER_MSVC",
        "COMPILER_MSVC64",
        "PLATFORM_64BITS",
        "X64BITS",
        "WIN32",
        "WINDOWS",
        "META_IS_SOURCE2",
        "NOMINMAX",
        "_CRT_SECURE_NO_WARNINGS",
        "_CRT_SECURE_NO_DEPRECATE",
        "_CRT_NONSTDC_NO_DEPRECATE",
        "_MBCS")

    add_links(
        SDK .. "/lib/public/win64/tier0.lib",
        SDK .. "/lib/public/win64/interfaces.lib")

    -- user32 for the window and its message loop, psapi for module enumeration.
    add_links("d3d11", "dxgi", "psapi", "user32")
