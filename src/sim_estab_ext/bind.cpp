#include <nanobind/nanobind.h>

#include <sim_estab/lib.h>
#include <sim_estab/log.h>

namespace nb = nanobind;

NB_MODULE(_if, m) {
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

    // Bind log_error exception (inherit from RuntimeError)
    static nb::exception<sim_estab::core::log::log_error> log_error_exc(log_module, "log_error", PyExc_RuntimeError);

    // Bind logging functions
    log_module.def("log_init", &sim_estab::core::log::log_init,
                   "Initialize the logging system");
    log_module.def("log_deinit", &sim_estab::core::log::log_deinit,
                   "Deinitialize the logging system");
    log_module.def("log_is_init", &sim_estab::core::log::log_is_init,
                   "Check if the logging system is initialized");
    log_module.def("enable_console", &sim_estab::core::log::enable_console,
                   "Enable console logging output");
    log_module.def("disable_console", &sim_estab::core::log::disable_console,
                   "Disable console logging output");

    // Keep existing bindings
    m.def("run", [](){});

    // init fn
    m.def("init", [](){});

    // deinit fn
    m.def("deinit", [](){});
}