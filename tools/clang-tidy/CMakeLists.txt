add_library(UcoClangTidy MODULE UcoClangTidy.cpp)
target_compile_features(UcoClangTidy PRIVATE cxx_std_20)
target_include_directories(UcoClangTidy SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)
target_compile_definitions(UcoClangTidy PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(UcoClangTidy PRIVATE
    clangTidy
    clangASTMatchers
)
set_target_properties(UcoClangTidy PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_executable(UcoClangd
    UcoClangdMain.cpp
    UcoClangTidy.cpp
)
target_compile_features(UcoClangd PRIVATE cxx_std_20)
target_include_directories(UcoClangd SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)
target_compile_definitions(UcoClangd PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(UcoClangd PRIVATE
    clangdMain
    clangTidy
    clangASTMatchers
)
set_target_properties(UcoClangd PROPERTIES
    OUTPUT_NAME "uco-clangd"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
