add_library(h2front
  src/Diagnostics.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/AstPrinter.cpp
  src/Loader.cpp
  src/Resolver.cpp
  src/Toml.cpp
  src/Project.cpp
)
target_include_directories(h2front PUBLIC include)
target_compile_options(h2front PRIVATE -Wall -Wextra -Wpedantic)

add_executable(h2c src/main.cpp)
target_link_libraries(h2c PRIVATE h2front)
target_compile_options(h2c PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(h2c PRIVATE H2_STDLIB_DIR="${PROJECT_SOURCE_DIR}/lib")

add_executable(hy src/hy.cpp)
target_link_libraries(hy PRIVATE h2front)
target_compile_options(hy PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(hy PRIVATE H2_STDLIB_DIR="${PROJECT_SOURCE_DIR}/lib")
