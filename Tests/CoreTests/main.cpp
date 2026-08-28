// main.cpp
//
// Entry point for the CoreTests binary. Every AKZ_TEST(...) registered in
// this translation unit's siblings runs; see TestFramework.h.

#include "TestFramework.h"

int main() {
    return akztest::runAll();
}
