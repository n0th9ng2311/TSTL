set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO n0th9ng2311/TSTL
    REF v0.1.0
    SHA512  97d83a8e50c53a198bef890a67961378acbcfa93c1d400979dc1a807fff35db320d274064c2938846f801af9fcfceca7e498daf8a3320c6796ae26f1018cd02e
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DTSTL_BUILD_TESTING=OFF
        -DTSTL_BUILD_BENCHMARKS=OFF
)


vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME TSTL_P CONFIG_PATH share/TSTL_P)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
