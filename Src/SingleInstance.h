#pragma once
#include <string>

namespace zdock {

	/// <summary>
	/// 单实例守卫。
	///
	/// ⚠ 为什么不用 Ling::App::refuseSecondInstance()：
	///   它靠 `FindWindow(L"STATIC", appID)` 判定。appID 来自
	///   `App::App() : appID{ COMPILE_TIME_RAND_STR(6) }` —— 而 `App()` 是
	///   **编译进预编译静态库 Ling.lib 的**。也就是说那个"随机"种子
	///   （__TIME__ / __COUNTER__）取自**库自己的编译时刻**，一旦 Ling.lib 打出来
	///   就固化了，跟链接它的程序是哪个、什么时候编的**毫无关系**。
	///
	///   实测：ZPin_2.7.0.exe 与 ZDock.exe 链接同一个 ling-v1.3.0 发布包，
	///   两边运行期打出来的 appID 都是同一个值（如 `Ling_Tf9K8M`）。
	///   于是两个程序都在 message-only 空间里占
	///   `类名 STATIC + 标题 Ling_Tf9K8M` 这一个槽位：
	///   后启动的一方必然 FindWindow 命中先启动的一方，给自己
	///   PostMessage(WM_APP+1) 之后 App::exit(0) = ExitProcess 直接杀进程 ——
	///   连"second instance"那行日志都来不及写（ZPin 与 ZDock 互相冲突就是这么来的）。
	///   顺带一提，WM_APP+1 恰好等于 ZPin 托盘菜单"关闭所有快捷键"的 id（165），
	///   所以误投的消息还会顺手切掉 ZPin 那个开关。
	///
	/// 改用命名 Mutex：名字里带程序自己的前缀，天然与别的程序隔离；
	/// 也顺带绕开了"HWND_MESSAGE 窗口不进常规枚举"这类平台细节。
	/// </summary>
	class SingleInstance
	{
	public:
		/// <summary>名字里空着不填 —— 由实现填上一个带 ZDock 前缀的唯一名。</summary>
		SingleInstance() = default;
		~SingleInstance();

		SingleInstance(const SingleInstance&) = delete;
		SingleInstance& operator=(const SingleInstance&) = delete;

		/// <summary>
		/// 尝试占住单实例。已有同程序的实例在跑时返回 false
		/// （此时内部句柄已关掉，调用方直接退出即可）。
		/// </summary>
		bool acquire();

		bool held() const { return mutex != nullptr; }

	private:
		void* mutex{ nullptr };   // HANDLE，用 void* 免得头文件拖进 Windows.h
	};

} // namespace zdock
