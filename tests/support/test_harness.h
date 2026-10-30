#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <sstream>

namespace nirq::test {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> func;
};

class TestRegistry {
 public:
  static TestRegistry& instance() {
    static TestRegistry reg;
    return reg;
  }

  void add(std::string suite, std::string name, std::function<void()> func) {
    tests_.push_back({std::move(suite), std::move(name), std::move(func)});
  }

  const std::vector<TestCase>& tests() const { return tests_; }

 private:
  std::vector<TestCase> tests_;
};

struct TestRegistrar {
  TestRegistrar(std::string suite, std::string name, std::function<void()> func) {
    TestRegistry::instance().add(std::move(suite), std::move(name), std::move(func));
  }
};

class TestFailure : public std::exception {
 public:
  TestFailure(std::string msg) : msg_(std::move(msg)) {}
  const char* what() const noexcept override { return msg_.c_str(); }
 private:
  std::string msg_;
};

#define TEST(suite, name) \
  static void test_##suite##_##name(); \
  static ::nirq::test::TestRegistrar registrar_##suite##_##name(#suite, #name, test_##suite##_##name); \
  static void test_##suite##_##name()

#define ASSERT_TRUE(cond) \
  do { \
    if (!(cond)) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": ASSERT_TRUE failed: " #cond; \
      throw ::nirq::test::TestFailure(ss.str()); \
    } \
  } while (0)

#define ASSERT_FALSE(cond) \
  do { \
    if (cond) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": ASSERT_FALSE failed: " #cond; \
      throw ::nirq::test::TestFailure(ss.str()); \
    } \
  } while (0)

#define ASSERT_EQ(a, b) \
  do { \
    if (!((a) == (b))) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": ASSERT_EQ failed: " #a " == " #b \
         << " (" << (a) << " != " << (b) << ")"; \
      throw ::nirq::test::TestFailure(ss.str()); \
    } \
  } while (0)

#define ASSERT_NE(a, b) \
  do { \
    if ((a) == (b)) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": ASSERT_NE failed: " #a " != " #b; \
      throw ::nirq::test::TestFailure(ss.str()); \
    } \
  } while (0)

#define EXPECT_TRUE ASSERT_TRUE
#define EXPECT_FALSE ASSERT_FALSE
#define EXPECT_EQ ASSERT_EQ
#define EXPECT_NE ASSERT_NE

}  // namespace nirq::test
