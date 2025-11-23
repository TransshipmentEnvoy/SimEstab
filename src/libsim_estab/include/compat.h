#ifndef LIBSIM_ESTAB__COMPAT_H
#define LIBSIM_ESTAB__COMPAT_H

// For compat reasons, g++14 can not handle header export well.
// Using a hybrid approach: export module + include headers.
// Standard library headers
#include <functional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

// Boost
#include <boost/log/core.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/keywords/severity.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#endif