# Writes a header with a hash of the codegen sources (script mode).
#   -DSOURCE_DIRS=<dir>;<dir>  directories whose .cpp/.h/.inc files are hashed
#   -DOUTPUT=<header>
#
# project_recompiler fingerprints generated modules with it, so a codegen change
# regenerates them while a relink of the tool that leaves the codegen alone
# (for example a runtime change) does not.
set(files "")
foreach(dir IN LISTS SOURCE_DIRS)
  file(GLOB_RECURSE found "${dir}/*.cpp" "${dir}/*.h" "${dir}/*.inc")
  list(APPEND files ${found})
endforeach()
list(SORT files)
set(combined "")
foreach(f IN LISTS files)
  file(SHA1 "${f}" digest)
  string(APPEND combined "${digest}\n")
endforeach()
string(SHA1 hash "${combined}")
set(content "#pragma once\n#define REXCODEGEN_SOURCE_HASH \"${hash}\"\n")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
  if(old STREQUAL content)
    return()
  endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
