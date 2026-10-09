# Writes a text file (library/rewrites.txt, data/steam-gpu-share.txt) as a C++ string (sopt::<VAR>, default
# kBuiltinLibrary), in pieces below MSVC's 16 KB limit per string literal.
# Usage: cmake -DIN=... -DOUT=... [-DVAR=name] -P embed_library.cmake
if(NOT VAR)
  set(VAR kBuiltinLibrary)
endif()
file(READ "${IN}" text)
string(LENGTH "${text}" len)
set(body "")
set(pos 0)
while(pos LESS len)
  string(SUBSTRING "${text}" ${pos} 8000 piece)
  string(APPEND body "R\"sopt_lib(${piece})sopt_lib\"\n")
  math(EXPR pos "${pos} + 8000")
endwhile()
if(body STREQUAL "")
  set(body "\"\"")
endif()
file(WRITE "${OUT}" "// Generated from ${IN} by cmake/embed_library.cmake.\nnamespace sopt {\nextern const char* const ${VAR};\nconst char* const ${VAR} =\n${body};\n}  // namespace sopt\n")
