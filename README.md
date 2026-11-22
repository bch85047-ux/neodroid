# NeoDroid

**面向 iOS ARM64 的 Android 用户态执行运行时**

NeoDroid 不是 QEMU，不是 UML，不翻译 ARM64 指令。
它在 ARM64 iPhone/iPad 上直接运行 Android ARM64 ELF、AOT oat 和 ART 代码；只在 Linux syscall 边界进入宿主兼容层。

## 设计目标

- Android 15/16 ARM64 用户态直接执行
- 3 秒级冷启动（AOT boot image + 分层 rootfs）
- Metal GPU 直连、60/120Hz ProMotion
- 不依赖 JIT entitlement、调试器、StikDebug 或电脑
- Rootfs 可更新、manifest 可验证、COW 用户数据

## 项目结构

```text
runtime/           C++17 Android Linux ABI 兼容运行时
  include/          公共头文件
  src/              ELF Loader / VFS / syscall / binder / scheduler
host/               iOS Objective-C++ 宿主适配层
  NeoHost.mm        Metal / UIKit / AVFoundation 桥接
  NeoHost.h
gpu/                Vulkan/GLES → Metal 转发层
android/            rootfs manifest、启动配置、系统属性
 tools/             rootfs 分层打包、AOT 预热、manifest 生成
 docs/              架构、ABI、性能设计
 tests/             runtime 单元测试
```

## 运行模型

```text
Android ARM64 ELF
       │ normal instructions: native direct execution
       ▼
SVC #0 syscall trap
       ▼
NeoRuntime::dispatch_syscall()
       ├─ fast path: futex / clock_gettime / gettid / epoll
       ├─ VFS: /system /data /proc /dev
       ├─ IPC: Binder / Unix sockets
       ├─ GPU: virtio-gpu → IOSurface → Metal
       └─ Host HAL: audio / camera / location / input
```

## 性能原则

1. **同架构零翻译**：Android arm64-v8a → Apple ARM64。
2. **AOT 预热**：boot.oat、services.odex、SystemUI oat 在构建阶段生成。
3. **syscall 分级**：高频 syscall 纯用户态快速返回；慢路径进 host service。
4. **零拷贝图形**：共享 IOSurface，不做 RGBA CPU readback。
5. **按需 rootfs**：system/framework/ui/vendor 分层，启动优先加载 boot-critical 页。

## 当前 P0

- ELF64/AArch64 头解析
- guest 地址空间布局
- syscall 编号/分发表
- 虚拟文件系统接口
- fast syscall API
- manifest 验证器
