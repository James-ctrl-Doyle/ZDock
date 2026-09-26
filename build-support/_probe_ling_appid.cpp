// 只做一件事：把 Ling::App::appID 打出来。
//
// 用途：验证 Ling v1.3.1 的「appID 按 exe 路径哈希」修复。
//   v1.3.0 时 appID 是**编译期常量**编译进 Ling.lib 的，所以链接同一个包的任何程序
//   appID 都相同 —— 这正是 "ZPin 与 ZDock 互相把对方当成第二实例" 的根因。
//   v1.3.1 改成按 `GetModuleFileNameW` 的路径做 FNV-1a 哈希。
//
// ⚠ 必须**运行期**读（别去扒 exe 字符串）：appID 是 std::wstring，运行期堆分配出来的，
//   静态扒文件搜不到、据此下的结论会是错的。这条弯路已经踩过一次。

#include <include/App.h>

#include <cstdio>

int main()
{
	if (!Ling::init()) {
		std::fwprintf(stderr, L"Ling::init() failed\n");
		return 2;
	}
	Ling::App* app = Ling::App::get();
	if (!app) {
		std::fwprintf(stderr, L"Ling::App::get() returned null\n");
		return 3;
	}
	std::wprintf(L"appID=%ls\n", app->appID.c_str());
	return 0;
}
