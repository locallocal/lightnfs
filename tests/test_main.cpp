#include "mini_test.hpp"

int main(int argc, char** argv) {
  return minitest::run_all(argc > 1 ? argv[1] : nullptr);
}
