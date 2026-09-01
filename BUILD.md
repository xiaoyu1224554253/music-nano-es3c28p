# 编译说明

## 环境准备

打开 PowerShell 后先导出 ESP-IDF 工具链：

```powershell
. C:\Users\123\esp\v5.5.5\esp-idf\export.ps1
```

确认 `idf.py` 可用：

```powershell
idf.py --version
```

## 编译（过滤无噪声输出）

仅显示警告、错误和最终链接结果，屏蔽 CMake 配置和逐文件编译进度：

```powershell
idf.py build 2>&1 | Select-String -Pattern "warning:|error:|FAILED|ninja: build stopped|Project build complete|Linking|ELF file|To flash" -SimpleMatch
```

`2>&1` 将 stderr 合并到 stdout，`Select-String` 只保留匹配行。

## 清空重构

更改 sdkconfig 后或遇到 CMake 缓存问题时：

```powershell
Remove-Item -Recurse -Force build; idf.py build 2>&1 | Select-String -Pattern "warning:|error:|FAILED|ninja: build stopped|Project build complete|Linking|ELF file|To flash" -SimpleMatch
```

## 仅编译 main 组件（快速验证）

```powershell
idf.py build 2>&1 | Select-String -Pattern "main.c|bt_a2dp|sys_serial|lvgl_task|audio_task|warning:|error:|FAILED|ninja: build stopped|Project build complete" -SimpleMatch
```

## 注意事项

- 路径分隔符用 `/` 或 `\` 均可，CMake 会自动处理
- `sdkconfig.defaults` 中的 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` 已启用
- 首次完整构建约 1252 步，增量编译仅编译变更的文件
