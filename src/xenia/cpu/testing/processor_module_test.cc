/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <algorithm>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::testing;

TEST_CASE("REMOVE_MODULE_KEEPS_MODULE_WITH_SAME_NAME", "[processor]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(CreateBackend());
  auto add = [&] {
    auto module = std::make_unique<TestModule>(
        processor.get(), "Patched", [](uint32_t) { return false; },
        [](hir::HIRBuilder&) { return false; });
    Module* added = module.get();
    REQUIRE(processor->AddModule(std::move(module)));
    return added;
  };
  Module* kept = add();
  Module* removed = add();
  processor->RemoveModule(removed);
  auto modules = processor->GetModules();
  CHECK(std::ranges::find(modules, kept) != modules.end());
  CHECK(std::ranges::find(modules, removed) == modules.end());
}
