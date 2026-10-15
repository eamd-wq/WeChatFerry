set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# 与现有 Codec.lib 及上游工程保持同一 MSVC 工具集，避免混用 LTCG 对象。
set(VCPKG_PLATFORM_TOOLSET v142)
# vcpkg 会清理构建环境，显式保留已校验的 pkg-config 路径。
set(VCPKG_ENV_PASSTHROUGH PKG_CONFIG)
