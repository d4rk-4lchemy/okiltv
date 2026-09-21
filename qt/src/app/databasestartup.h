#pragma once

#include <functional>

namespace OKILTV::App {

// Runs startup storage work on a worker while the GUI processes events. The
// task calls rebuildStarted only when a migration/cleanup is actually needed.
// Exceptions are rethrown on the caller's GUI thread after the worker finishes.
void runDatabaseStartup(const std::function<void(const std::function<void()> &)> &task);

} // namespace OKILTV::App
