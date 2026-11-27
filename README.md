# PcRemoteController
用于辅助远程控制电脑，主要通过 SSH 执行一条中文命令完成操作。

## 构建
使用 Visual Studio 2022 的 MSBuild 构建（x64 Release）：

```
MSBuild.exe PcRemoteController.vcxproj /p:Configuration=Release /p:Platform=x64
```

输出：`x64\Release\PcRemoteController.exe`。

## 使用
```
PcRemoteController.exe <命令>
```

| 功能 | 命令 |
| --- | --- |
| 增加音量 | `音量增加 [数值]`，默认 5 |
| 减少音量 | `音量减少 [数值]`，默认 5 |
| 设置音量 | `设置音量 <0-100>`，默认 50 |
| 播放/暂停 | `暂停` / `播放` |
| 输入文字 | `输入文字 <文本>`，输入到当前前台窗口 |
| 粘贴文字 | `粘贴文字 <文本>`，通过剪贴板粘贴（会覆盖剪贴板） |
| 复制文字 | `复制文字 <文本>`，只写入剪贴板，不执行粘贴 |
| 关闭键盘 | `关闭键盘`（需要管理员权限） |
| 打开键盘 | `打开键盘`（需要管理员权限） |
| 休眠 | `休眠` |

命令使用子串匹配，可以带其他文字。例如：

```
PcRemoteController.exe 音量增加 10
```

文字类命令必须放在命令开头，文本会发送到当前前台窗口。Agent 以普通用户运行，无法向管理员权限窗口输入。复制成功时会在被控电脑右下角显示一个短暂的桌面提示（不进入通知中心）。

退出码：`0` 成功，`1` 执行失败，`2` 未知命令。

## 首次安装
播放/暂停和文字操作通过交互式会话中的常驻 Agent 完成，需要安装登录任务（管理员权限）：

```
PcRemoteController.exe --install-agent
```

键盘控制依赖开机恢复任务，安装一次即可（管理员权限）：

```
PcRemoteController.exe --install-keyboard-guard
```

## 诊断
```
PcRemoteController.exe --help                    # 查看全部命令
PcRemoteController.exe --agent-ping              # 检查 Agent 是否可用
PcRemoteController.exe --list-keyboards          # 列出键盘设备（详情见日志）
PcRemoteController.exe --uninstall-agent         # 卸载 Agent 登录任务
PcRemoteController.exe --uninstall-keyboard-guard # 卸载键盘恢复任务
```

详细日志写入 `D:\debug.txt`（GBK 编码）。`--debug` 会先清空日志。
