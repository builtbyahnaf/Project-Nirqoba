#include "test_harness.h"
#include <iostream>

using namespace std;

int main() {
  const auto& tests = nirq::test::TestRegistry::instance().tests();
  int passed = 0;
  int failed = 0;

  std::cout << "[==========] Running " << tests.size() << " unit tests.\n";

  for (const auto& t : tests) {
    std::cout << "[ RUN      ] " << t.suite << "." << t.name << "\n";
    try {
      t.func();
      std::cout << "[       OK ] " << t.suite << "." << t.name << "\n";
      ++passed;
    } catch (const nirq::test::TestFailure& ex) {
      std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << "\n"
                << "  " << ex.what() << "\n";
      ++failed;
    } catch (const std::exception& ex) {
      std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << "\n"
                << "  Unexpected exception: " << ex.what() << "\n";
      ++failed;
    } catch (...) {
      std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << "\n"
                << "  Unknown exception thrown\n";
      ++failed;
    }
  }

  std::cout << "[==========] " << tests.size() << " tests ran.\n";
  std::cout << "[  PASSED  ] " << passed << " tests.\n";
  if (failed > 0) {
    std::cerr << "[  FAILED  ] " << failed << " tests.\n";
    return 1;
  }
  return 0;
}
