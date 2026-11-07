#include "test_harness.h"
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
  const auto& tests = nirq::test::TestRegistry::instance().tests();
  int passed = 0;
  int failed = 0;
  std::string filter = argc > 1 ? argv[1] : "";

  std::cout << "[==========] Running " << tests.size() << " unit tests.\n";

  for (const auto& t : tests) {
    std::string full_name = t.suite + "." + t.name;
    if (!filter.empty() && full_name.find(filter) == std::string::npos && t.name.find(filter) == std::string::npos) {
      continue;
    }
    std::cout << "[ RUN      ] " << t.suite << "." << t.name << std::endl;
    try {
      t.func();
      std::cout << "[       OK ] " << t.suite << "." << t.name << std::endl;
      ++passed;
    } catch (const nirq::test::TestFailure& ex) {
      std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << std::endl
                << "  " << ex.what() << std::endl;
      ++failed;
    } catch (const std::exception& ex) {
      std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << std::endl
                << "  Unexpected exception: " << ex.what() << std::endl;
      ++failed;
    } catch (...) {
      std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << std::endl
                << "  Unknown exception thrown" << std::endl;
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
