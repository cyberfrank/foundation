
workspace "workspace"
    configurations { "Debug", "Release" }
    language "C"
    flags {
        "FatalWarnings", 
        "MultiProcessorCompile"
    }
    warnings "Extra"
    inlining "Auto"
    sysincludedirs { "" }
    editAndContinue "Off"
    targetdir "bin"
    location "bin"
    includedirs "src"
    characterset "MBCS" 

filter "system:windows"
    platforms { "Win64" }
    systemversion("latest")

filter "platforms:Win64"
    defines { 
        "OS_WINDOWS", 
        "_CRT_SECURE_NO_WARNINGS"
    }
    staticruntime "On"
    architecture "x64"
    disablewarnings {
        "4057", -- Slightly different base types.
        "4100", -- Unused formal parameter.
        "4152", -- Conversion from function pointer to void *.
        "4200", -- Zero-sized array. Valid C99.
        "4201", -- Nameless struct/union. Valid C11.
        "4204", -- Non-constant aggregate initializer. Valid C99.
        "4206", -- Translation unit is empty. Might be #ifdefed out.
        "4214", -- Bool bit-fields. Valid C99.
        "4221", -- Pointers to locals in initializers. Valid C99.
        "4702", -- Unreachable code.
        "4706", -- Assignment within conditional. Valid C99.
    }
    linkoptions { "/ignore:4099" }

filter "configurations:Debug"
    defines { "DEBUG_MODE", "DEBUG" }
    symbols "On"

filter "configurations:Release"
    defines { "RELEASE_MODE", "RELEASE" }
    optimize "On"

project "foundation-lib"
    kind "StaticLib"
    targetname "foundation-lib"
    targetdir "bin/%{cfg.buildcfg}"
    files { "src/**.h", "src/**.c" }
