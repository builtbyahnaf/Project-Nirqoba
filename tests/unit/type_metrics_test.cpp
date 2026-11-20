// Chi-5b-i-b-2a: frontend type metrics.
//
// The dynamic array initializer needs an element size and alignment to emit its
// allocator call, and the value itself must be three words. This pins the numbers
// the lowering will use, and pins the refusal case: a type whose layout is not
// fixed reports zero rather than a plausible-looking wrong answer.

#include "test_harness.h"

#include "nirqoba/Common/Type.h"

using nirq::Type;

TEST(TypeMetricsTest, ADynamicArrayIsThreeWordsAndPointerAligned) {
  const Type array = Type::dynamic_array_type(Type::int_type());
  ASSERT_EQ(array.byte_size(), 24u);
  ASSERT_EQ(array.alignment(), 8u);
}

TEST(TypeMetricsTest, APointerIsOneWord) {
  ASSERT_EQ(Type::pointer_type(Type::int_type()).byte_size(), 8u);
  ASSERT_EQ(Type::pointer_type(Type::int_type()).alignment(), 8u);
}

TEST(TypeMetricsTest, ScalarSizesAndAlignmentFollowTheirWidth) {
  ASSERT_EQ(Type::i8_type().byte_size(), 1u);
  ASSERT_EQ(Type::i8_type().alignment(), 1u);
  ASSERT_EQ(Type::i32_type().byte_size(), 4u);
  ASSERT_EQ(Type::i32_type().alignment(), 4u);
  ASSERT_EQ(Type::i64_type().byte_size(), 8u);
  ASSERT_EQ(Type::i64_type().alignment(), 8u);
  ASSERT_EQ(Type::int_type().byte_size(), 8u);
}

TEST(TypeMetricsTest, AnOwnedStringIsAWord) {
  ASSERT_EQ(Type::owned_string_type().byte_size(), 8u);
  ASSERT_EQ(Type::owned_string_type().alignment(), 8u);
}

TEST(TypeMetricsTest, ATypeWithoutAFixedLayoutReportsZero) {
  // A tuple's size depends on its element list, which this metric does not walk;
  // answering zero keeps callers from treating an unknown layout as known.
  const Type tuple = Type::tuple_type({Type::int_type(), Type::i8_type()});
  ASSERT_EQ(tuple.byte_size(), 0u);
  ASSERT_EQ(tuple.alignment(), 0u);
  ASSERT_EQ(Type::error().byte_size(), 0u);
}
