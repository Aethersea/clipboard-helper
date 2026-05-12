// Test runner entry point.
//
// Individual test files use `TEST(Suite, Name) { ... }` to register cases
// against ::test_lite::Registry(); this main just walks them.

#include "test_lite.h"

int main(int argc, char** argv) {
    return ::test_lite::RunAll(argc, argv);
}
