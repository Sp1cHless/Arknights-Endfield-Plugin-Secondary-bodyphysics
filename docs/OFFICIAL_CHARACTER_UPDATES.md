# 官方角色数据的打包与更新

官方角色的技术数据来自 `SecondaryMotion/data/characters.default.json`，推荐动作参数来自 `SecondaryMotion/presets/Default.json`。`assemble_one.bat` 分别把它们复制为发布包中的 `data/characters.default.template.json` 和 `presets/Default.template.json`；中英文包使用同一份参数，中文打包步骤仅替换已知角色的显示名称。

SI 的 id 为 `chr_0036_jsspsi`，显示名称为 `Si`。两个发布源文件均已纳入完整数据：骨骼 `R_breast_01_jnt` / `L_breast_01_jnt`，预设轴为 Z、正方向；walk/run/sprint 的频率分别为 1.7/2.7/3.4 Hz。其 gait、envelope、jump 和 native_amplify 参数均使用保存的 SI 数据。

Manager 启动时，`OfficialCharactersMigration` 会在载入用户模型和旧版 Jump 迁移之前检查新出现的官方角色 id：

- 用户技术库缺少该 id：复制发布技术模板中的完整角色对象。
- 用户某个正式预设缺少该 id：复制发布 Default 模板中的完整角色对象，包括启用状态、模式、轴及动作参数。Default、User 和自定义预设分别检查。
- 已存在的角色对象保留原样，包括用户自己录入的 SI。
- 每个官方 id 只处理一次，记录在游戏数据目录的 `data/.official_characters_v1.json`。后续用户主动删除角色或预设项，不会在每次启动时重新加入；未来发布新增 id 仍可自动补入。
- 修改前备份目标文件；先解析所有目标文件，遇到损坏数据不进行部分写入，也不记录迁移完成。

这项机制独立于旧的 `.jump_defaults_v3` 和 `.typhoeus_official_v1` 标记。以前执行过旧迁移的用户也能收到新 SI。运行时只读取正式工作数据，发布模板本身不会成为可选择的预设。

验证命令（仓库根目录）：

```powershell
dotnet run --project tests/OfficialCharacterMigrationTests.csproj -c Release -- . artifacts/official-character-tests
python verify_ui_serialization.py
```

2026-10-18 验证：25 项迁移测试通过，Manager 构建通过，序列化/打包入口检查通过。测试使用隔离目录，覆盖缺失记录、已有自定义数据、DB/预设独立缺失、旧迁移标记、重复启动、迁移后删除、未来新增角色、开发模板回退和损坏预设。中英文模板复制及中文本地化后，SI 的技术与动作对象均与发布源一致。此处没有创建或发布正式发行包。
