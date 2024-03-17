-- sudo apt-get install uuid-dev
-- dz-captcha asmjit and blend2d should be in the same parent folder

workspace "dz"
  configurations {"Debug", "Release"}
  location "build"
  filter "configurations:Debug"
    symbols "On"
    targetsuffix "_debug"

project "dz-captcha"
  language "C++"
  cppdialect "C++20"
  kind "ConsoleApp"

  targetdir "."
  includedirs {"include", "."}
  libdirs {"lib"}
  files {"*.cpp"}
  links {"hv", "uuid", "thorvg"}

  vectorextensions "AVX"
  buildoptions {"-mpclmul"}
  defines {"BUILD_VERSION=\"1.0\""}