#pragma once

#include <mutex>

namespace seds {

using RouterMutex = std::recursive_mutex;
using RouterLock = std::scoped_lock<RouterMutex>;

}  // namespace seds
