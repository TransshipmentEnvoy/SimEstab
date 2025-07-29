#include <nanobind/nanobind.h>

#include <sim_estab/lib.h>

NB_MODULE(_if, m) {
    m.def("run", [](){});

    // init fn
    m.def("init", [](){});

    // deinit fn
    m.def("deinit", [](){});
}