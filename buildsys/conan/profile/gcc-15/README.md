# gcc-15 fallback profiles

These are **not** the active profile set. They pin `compiler.version=15` /
`compiler.cppstd=gnu23` and are kept only as a fallback for environments that
still have GCC 15.

The active profiles are `buildsys/conan/profile/linux_*` — GCC 16 / `gnu26`,
which is what the CMake targets require (`CXX_STANDARD 26` plus `-freflection`
for P2996 reflection). GCC 15 cannot build the project at that standard, so
these profiles will not produce a working `libsim_estab` as things stand.
