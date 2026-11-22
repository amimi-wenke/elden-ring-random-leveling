# 《艾尔登法环》随机升级（Random Leveling）

适用于 Steam 国际版 1.17.2（`eldenring.exe` 2.7.2.0）。前置依赖为 Mod Engine 3，已用 0.13.0 验证配置。 [Mod Engine 3 官方发布页](https://github.com/garyttierney/me3/releases/tag/v0.13.0)

解压时保留整个 `EldenRing-RandomLeveling` 文件夹。参数放在本 MOD 的 `random-leveling-assets`，法魂（The Convergence）等 MOD 可以继续使用自己原有的目录和启动脚本。

- 单独使用时双击 `Start-offline.cmd`。
- 和无缝联机组合时，由所有玩家安装同版本的随机升级 MOD、Mod Engine 3 和无缝联机，然后双击 `Start-seamless.cmd`。脚本默认查找游戏目录下的 `SeamlessCoop\ersc.dll`，也可以用 `-SeamlessDll` 指定已安装的 DLL。联机已组合加载的版本为 2.0.3，双人玩法同步仍待测试。 [无缝联机官方发行页](https://github.com/yuiamoroll/EldenRingSeamlessCoopRelease/releases/tag/v2.0.3)

脚本会在 Steam 库里查找游戏，并从系统 PATH 或游戏目录的 `me3\Windows` 查找加载器。自定义安装路径也可以手动指定。

```cmd
Start-offline.cmd -Game "D:\SteamLibrary\steamapps\common\ELDEN RING\Game" -Loader "C:\ModEngine3\me3.exe"
```

## 玩法

- 角色只能靠击杀升级，在赐福用卢恩升级会被撤销并退还卢恩（卢恩仍可用于购物）。普通敌人和精英怪 1 点，Boss 20 点，攒够点数升 1 级并随机加 1 点属性。10 级前每级 5 点，之后每 5 级多 1 点（20 级 7 点，60 级 15 点，100 级 23 点，150 级 33 点）。属性随机，不一定加在需要的地方，所以等级定得偏高：估算本体主线通关约 190 级，清完全部 Boss 和大部分敌人约 275 级，全清约 345 级。Boss 以 NpcParam 的 Boss 卢恩标记识别。鹿、羊、鸟、野猪、乌龟、兔子、圣甲虫等小动物不给点数。
- 普通地图敌人按出生位置记录，每次击杀后的记录会写入 `state`。传送、玩家死亡重生和退出重进时，MOD 在下次载入地图敌人时重新应用死亡记录。被清理的敌人不会再次奖励卢恩和掉落。Boss 的转阶段及剧情重生仍由游戏脚本处理。
- 331 件原本需要刷怪的武器、防具和护符设为必掉，每件装备按存档记录一次领取。
- 死亡次数按角色保留。敌人进入加载范围后，其最大生命值按 `基础生命值 × (1 + 0.05 × 死亡次数)` 计算。基础生命值 1,000 的敌人在一次、两次和三次死亡后分别为 1,050、1,100 和 1,150。联机时以房主的世界记录和死亡次数为准，访客只处理自己的属性奖励。
- 无缝联机时，单个玩家倒下不算惩罚。房主世界里所有在场玩家同时阵亡（团灭）才计 1 次，敌人血量按团灭次数提高。单人游戏仍按玩家死亡次数计算。
- 无缝联机不会同步存档，每人用自己的存档。作为客人进入房主世界时，身边被击杀的敌人（不论谁打死的）会记进你自己的角色记录，回到自己世界后这些敌人同样不会复活。击杀加点和升级进度也存进自己的记录，离开房主世界不会丢失。同一个敌人对同一个角色只给一次升级点，在自己世界已杀过的敌人，到别人世界再杀不再给点。客人不会改动房主世界里的敌人。

## 存档和测试状态

单独使用时 Mod Engine 3 使用 `ER0000.onelife`。联机入口向 Mod Engine 3 指定 `ER0000.onelifecoop.co2`，这两个存档名沿用 MOD 改名前的名称，以免已有角色找不到。实际存档名可能受无缝联机的存档扩展名设置影响。初次启动时 Mod Engine 3 可能生成已有存档的副本。请选「开始新游戏」新建角色进行测试。移动 MOD 后如果要继续同一角色，随包迁移 `state` 目录。角色本身保存在游戏存档目录。

自动测试覆盖了属性上限、血量线性累加、角色记录读写，以及敌人更换运行时地址后按出生位置应用死亡记录。尚未用新角色实测传送、死亡和读档后的敌人状态。剧情触发的临时敌人及没有稳定地图标识的敌人可能刷新。

## 构建资料

- `build_runtime.ps1` 构建玩法 DLL，工具链为 w64devkit。
- `build_regulation.py` 使用 [Elden Ring Super Randomizer 解析器](https://github.com/Eratas/elden-ring-super-randomizer/tree/72b70afda8f51e400937fccc82882a8b8041fdb8/tools) 生成游戏参数。
- 运行时布局参考了 [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs)、[Minecraft-Ring](https://github.com/siddoff/Minecraft-Ring) 和 [NativeTweaks](https://github.com/ArialSenki/NativeTweaks)。运行地址已与 1.17.2 国际版游戏文件核对。
