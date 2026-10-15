#pragma once
// 开机自启动：**用计划任务实现，不用注册表 Run**。
//
// 程序抓包必须管理员权限，而 exe 清单声明了 requireAdministrator，
// 走注册表 Run 的话每次登录都会弹一次 UAC（用户会烦）。
// 计划任务在创建时就把"最高权限"这件事定下来了（/RL HIGHEST），
// 之后每次由任务拉起进程时系统直接给提权令牌，**登录时不再弹 UAC**。
//
// 任务名固定为 TrafficScope，触发条件是 ONLOGON（用户登录时），
// 只在创建者登录时运行（需要交互桌面显示浮框）。
class QString;

namespace ts {

// 任务是否存在（用 schtasks /Query 的退出码判断，0 = 存在）
bool autoStartEnabled();

// 创建 / 删除任务。err 可选，失败时拿到 schtasks 的输出便于排查（例如未提权）。
bool setAutoStart(bool on, QString* err = nullptr);

}  // namespace ts
