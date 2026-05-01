#ifndef LIBSIM_ESTAB__COMPAT_H
#define LIBSIM_ESTAB__COMPAT_H

// For compat reasons, g++14 can not handle header export well.
// Using a hybrid approach: export module + include headers.
//
// With the opaque wrapper implementation, Boost.Log headers are no longer
// required in user code. The wrapper classes (record, record_ostream, logger,
// logger_mt) completely hide Boost.Log types behind fast pimpl.

// Standard library headers needed for module interface
#include <cstddef>
#include <functional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#endif