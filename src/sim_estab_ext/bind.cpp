// nanobind first: it includes Python.h, which must precede the standard headers
// because it defines _POSIX_C_SOURCE and _XOPEN_SOURCE
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

// Standard library headers
#include <functional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sim_estab/compat.h>
import sim_estab;
// #include <sim_estab/macro.h>

namespace nb = nanobind;

NB_MODULE(_if, m) {
    // Bind the root of the exception family (inherit from RuntimeError). It is bound before
    // any derived exception: nanobind tries the newest translator first, so a derived
    // exception must register after its base to be raised as itself.
    static nb::exception<sim_estab::core::error::sim_estab_error> sim_estab_error_exc(m, "SimEstabError",
                                                                                      PyExc_RuntimeError);

    // Create log submodule
    nb::module_ log_module = m.def_submodule("log", "Logging system for SimEstab");

    // Bind severity_level enum
    nb::enum_<sim_estab::core::log::severity_level>(log_module, "severity_level")
        .value("trace", sim_estab::core::log::severity_level::trace)
        .value("debug", sim_estab::core::log::severity_level::debug)
        .value("info", sim_estab::core::log::severity_level::info)
        .value("warning", sim_estab::core::log::severity_level::warning)
        .value("error", sim_estab::core::log::severity_level::error)
        .value("critical", sim_estab::core::log::severity_level::critical);

    // Bind log_error exception as LogError (inherit from SimEstabError)
    static nb::exception<sim_estab::core::log::log_error> log_error_exc(log_module, "LogError",
                                                                        sim_estab_error_exc.ptr());

    // Bind logging functions
    log_module.def("log_init", &sim_estab::core::log::log_init, "Initialize the logging system");
    log_module.def("log_deinit", &sim_estab::core::log::log_deinit, "Deinitialize the logging system");
    log_module.def("log_is_init", &sim_estab::core::log::log_is_init, "Check if the logging system is initialized");
    log_module.def("enable_console", &sim_estab::core::log::enable_console, "Enable console logging output");
    log_module.def("disable_console", &sim_estab::core::log::disable_console, "Disable console logging output");

    // Add SIM_ESTAB_LOG binding for direct logging from Python
    log_module.def(
        "log",
        [](const std::string& channel, sim_estab::core::log::severity_level level, const std::string& message) {
            sim_estab::core::log::sim_estab_log(channel, level, message);
        },
        nb::arg("channel"), nb::arg("level"), nb::arg("message"),
        "Log a message to the SimEstab logging system.\n\n"
        "Args:\n"
        "    channel (str): Hierarchical channel name (e.g., \"sim_estab.network.tcp\")\n"
        "    level (severity_level): Log level (trace, debug, info, warning, error, critical)\n"
        "    message (str): Message content to log");

    // Keep existing bindings
    m.def("run", []() {});

    // init fn
    m.def("init", []() {});

    // deinit fn
    m.def("deinit", []() {});
}