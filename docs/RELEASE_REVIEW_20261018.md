# v3.2.0 发布前检查

检查结果：未发现阻止本次打包的问题。正式 ZIP 留给用户生成。

本轮修正：

- Manager 和 Runtime 版本统一为 3.2.0；已构建的 EXE、Manager DLL、sbm.dll 文件版本均为 3.2.0.0。
- 发布脚本默认从 Manager 项目读取版本，避免本地旧 ZIP 导致版本号自动偏移；显式传入不一致版本时终止。
- 官方角色迁移测试接入 `verify.bat`；补充旧版迁移完整启动顺序和 Manager 实际数据载入验证。
- README 官方角色数量更新为 21，加入 SI、当前版本及验证说明。

验证结果：

| 检查 | 结果 |
| --- | --- |
| `verify.bat` | 427 项 C++ 测试、32 项 Manager 迁移测试、序列化、硬编码扫描、JSON 和完整 Runtime 构建通过 |
| 自包含 `win-x64` Manager Release 发布构建 | 通过；输出在 `artifacts/release-review-20261018/published-manager`，未生成发行 ZIP |
| 发布版本选择 | 默认、显式 v3.2.0、不一致版本拒绝三个分支通过；执行范围截止构建前 |
| SI 发布数据 | DB 和 Default 均包含完整对象；中英文模板中的 SI 与源数据一致 |
| 最近实机轨迹 | 见 `artifacts/motion-final-retest-20261018/REPORT.md`；本轮只更改版本、检查和文档，未再次修改动作算法 |

Manager 编译仍存在原有可空性警告，发布构建无错误。用户当前预设调参保留；打包读取工作目录里的 Default/User 源文件。SI 缺失时由新版 Manager 从发布 Default 模板补入，已有 SI 对象保持原样。

复核日志：仓库根目录的 `verify_release_review.log`、`verify_release_publish.log`。本轮未安装 DLL、未执行正式打包或上传。
