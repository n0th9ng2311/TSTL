set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO n0th9ng2311/TSTL
    REF v0.1.0
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME TSTL_P CONFIG_PATH share/TSTL_P)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
