# GPGPU 设备的 Meson 构建配置
#
# 这个文件告诉 Meson 构建系统如何编译 GPGPU 设备
# system_ss 是系统模拟器的源文件集合

# 基础 C 源文件 (始终编译)
gpgpu_c_sources = files(
    'gpgpu.c',
    'gpgpu_core.c',
)

# Vortex simx C++ 桥接层 (实验二)
# 当 CONFIG_GPGPU_VORTEX 启用时，编译 C++ 桥接层并链接 simx 库。
# 目前 CONFIG_GPGPU_VORTEX 默认关闭（需要先克隆 thirdparty/vortex 并编译）。
# 启用方式: ../configure --enable-gpgpu-vortex
gpgpu_vortex_sources = files(
    'gpgpu_core_vortex.cpp',
)

# 检查 Vortex 库是否存在
vortex_dir = meson.project_source_root() / 'thirdparty' / 'vortex'
vortex_lib = vortex_dir / 'build' / 'lib' / 'libsimx.a'

# Auto-detect: if Vortex lib exists, enable it
gpgpu_vortex_enabled = fs.exists(vortex_lib)
if gpgpu_vortex_enabled
    # Vortex 可用: 编译桥接层 + 链接 simx
    gpgpu_c_sources += gpgpu_vortex_sources
    gpgpu_c_args = ['-DCONFIG_GPGPU_VORTEX']
    gpgpu_link_args = [
        '-L' + (vortex_dir / 'build' / 'lib'),
        '-lsimx',
        '-lstdc++',
    ]
    gpgpu_include_dirs = include_directories(
        vortex_dir / 'simx' / 'include'
    )
    message('GPGPU: Vortex simx backend ENABLED')
else
    # Vortex 不可用: 编译桩桥接层 (stub, 返回 -1)
    # gpgpu.c 会自动回退到内置 C 解释器
    gpgpu_c_sources += gpgpu_vortex_sources
    gpgpu_c_args = []
    gpgpu_link_args = ['-lstdc++']
    gpgpu_include_dirs = []
    message('GPGPU: Vortex simx backend DISABLED (using built-in interpreter)')
    message('        To enable: clone vortex to thirdparty/vortex and rebuild')
endif

system_ss.add(when: 'CONFIG_GPGPU', if_true: [
    gpgpu_c_sources,
    gpgpu_include_dirs,
    declare_dependency(
        compile_args: gpgpu_c_args,
        link_args: gpgpu_link_args,
    ),
])
