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
| 复制图片 | `复制图片 -base64`（stdin）或 `复制图片 <路径>`（电脑上的图片文件） |
| 最新截图 | `最新截图 [目录]`，默认取用户目录 `latest.*` 或 `Pictures\PhoneScreenshots` 中最新的一张 |
| 关闭键盘 | `关闭键盘`（需要管理员权限） |
| 打开键盘 | `打开键盘`（需要管理员权限） |
| 休眠 | `休眠` |

文字类命令（`输入文字` / `粘贴文字` / `复制文字`）支持 `-stdin`：从标准输入读取 UTF-8 文本，支持多行，不经过 SSH 命令行的 shell 解析。

```
type 文本.txt | PcRemoteController.exe 复制文字 -stdin
```

Windows OpenSSH 默认由 cmd.exe 执行远程命令，命令行里的换行会在第一处被截断，因此多行文本请使用 `-stdin`。iOS 快捷指令“通过 SSH 运行脚本”可以把文本填在“输入”里，命令填 `PcRemoteController.exe 复制文字 -stdin`。

命令使用子串匹配，可以带其他文字。例如：

```
PcRemoteController.exe 音量增加 10
```

文字类命令必须放在命令开头，文本会发送到当前前台窗口。Agent 以普通用户运行，无法向管理员权限窗口输入。复制成功时会在被控电脑右下角显示一个短暂的桌面提示（不进入通知中心）。

退出码：`0` 成功，`1` 执行失败，`2` 未知命令。

## 手机截图到电脑剪贴板（iOS 快捷指令）

`复制图片 -base64` 从标准输入读取一张 Base64 编码的图片，交给交互式会话中的 Agent 解码并写入剪贴板。它配合 iOS 快捷指令的“通过 SSH 运行脚本”（Run Script Over SSH）动作使用，不需要在手机上安装 SFTP 工具：

1. iPhone 截图后打开“快捷指令”，新建一个快捷指令。
2. 添加“查找照片”，将相簿设为“截图”（Screenshots），按拍摄日期排序并限制为 1 张；也可以用“选择照片”手动选择。
3. 添加“Base64 编码”，将换行选项设为“无”。
4. 添加“通过 SSH 运行脚本”：
   - 主机和用户：与平时 SSH 登录电脑相同；
   - 命令：`D:\Code\PcRemoteController\x64\Release\PcRemoteController.exe 复制图片 -base64`；
   - 输入：上一步得到的 Base64 文本。
5. 添加“显示结果”，查看 `图片已复制到剪贴板（宽 × 高）` 或错误信息。

注意：

- 该动作必须在“快捷指令”App 内运行；从分享面板触发的后台运行器可能报 `This action could not be run with the current user interface`。可以先在 App 内运行，或先保存截图再用 `shortcuts://run-shortcut?name=...` 拉起。
- 首次连接需要在弹窗中确认主机指纹。认证推荐在动作里生成 ed25519 密钥，把公钥加入 Windows 的 `authorized_keys`；管理员账号默认读取 `C:\ProgramData\ssh\administrators_authorized_keys`，该文件的 ACL 只能保留 Administrators 和 SYSTEM。
- 不要直接把图片文件当二进制塞进标准输入，先做 Base64 编码；图片解码后限制为 24 MB、30 MP，超过时请先在快捷指令里缩放/压缩。
- 命令只会写剪贴板，不会自动粘贴；会覆盖剪贴板原有内容。同一用户下的其他程序本来就能读取剪贴板。
- 需要已安装并运行 Agent（见“首次安装”）。SSH 用户必须和桌面登录用户一致。

## 手机截图经 a-Shell / scp 导入（可选）

大图不想走 Base64 时，可以先用手机上的 SSH 客户端（例如 a-Shell 的 `scp`）把截图上传到电脑，再让电脑读取文件：

```sh
# 手机 a-Shell，上传到 SSH 用户主目录（C:\Users\<用户名>\latest.png）
scp ~/Documents/IMG_0001.PNG <用户名>@<电脑IP>:latest.png
```

然后在电脑或 SSH 里执行：

```
PcRemoteController.exe 最新截图
```

`最新截图` 默认查找两个位置，并取修改时间最新的一张：

- `%USERPROFILE%\latest.png`（以及 `latest.jpg` / `latest.jpeg` / `latest.bmp` / `latest.gif` / `latest.tif` / `latest.tiff`）
- `%USERPROFILE%\Pictures\PhoneScreenshots` 下的任意图片

也可以显式指定文件或目录：

```
PcRemoteController.exe 复制图片 C:\Users\<用户名>\latest.png
PcRemoteController.exe 最新截图 D:\PhoneScreenshots
```

图片文件限制 24 MB；CLI 读文件后交给交互式 Agent，Agent 不需要直接访问上传文件。scp 的 SSH 密钥、host key 和管理员 `administrators_authorized_keys` 的 ACL 与前面的注意事项相同。

## 首次安装
播放/暂停、文字操作和图片复制通过交互式会话中的常驻 Agent 完成，需要安装登录任务（管理员权限）：

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

详细日志写入 `D:\debug.txt`（GBK 编码，每行带本地时间戳）。`--debug` 会先清空日志。

每次常规命令调用还会向 `D:\calls.log` 追加一行审计记录（GBK）：时间、命令关键字、文字来源（`arg` / `stdin`，仅文字命令）、结果（`OK` / `FAIL` / `UNKNOWN`）和退出码。审计日志不记录文字正文。
