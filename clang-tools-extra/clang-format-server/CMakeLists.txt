set(LLVM_LINK_COMPONENTS
  support
  )

add_clang_executable(clang-format-server
  ClangFormatServer.cpp
      )

clang_target_link_libraries(clang-format-server
  PRIVATE
  clangBasic
  clangFormat
  clangRewrite
  clangToolingCore
  zmq
  )

set_source_files_properties(ClangFormatServer.cpp PROPERTIES COMPILE_FLAGS -fexceptions)

