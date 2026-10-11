// SPDX-License-Identifier: Apache-2.0
#include <exception>
#include <iostream>

#include "fixture_support.h"
int main() {
  try {
    materialization_test::ExactIngress();
    materialization_test::SelectionValues();
    materialization_test::RawDefectsAndLimits();
    std::cout << "Independent materialization semantic fixtures passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
