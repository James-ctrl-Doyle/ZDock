#include "SingleInstance.h"
#include "Log.h"

#include <Windows.h>
#include <format>

namespace zdock {

	namespace {

		// 名字带程序自己的前缀：与同版本 Ling 生成的 "Ling_" 之类的通用名天然隔离。
		// 放进 Local\ 命名空间 —— 每个登录会话一个，多用户同时登录时互不干扰
		// （放 Global\ 需要 SeCreateGlobalPrivilege，普通权限下建不出来）。
		constexpr const wchar_t* kMutexName = L"Local\\ZDock.SingleInstance.{6B1E7A3C-5D42-4F90-9E11-2C7A8B4D6F01}";

	} // namespace

	SingleInstance::~SingleInstance()
	{
		if (mutex) {
			CloseHandle(static_cast<HANDLE>(mutex));
			mutex = nullptr;
		}
	}

	bool SingleInstance::acquire()
	{
		if (mutex) return true;   // 已经占住了，重复调用是幂等的

		HANDLE h = CreateMutexW(nullptr, TRUE, kMutexName);
		if (!h) {
			// 建不出来（极端情况）就当没有单实例保护，不要因此拒绝启动
			log(std::format(L"[single] CreateMutex 失败 GetLastError={}", GetLastError()));
			return true;
		}

		if (GetLastError() == ERROR_ALREADY_EXISTS) {
			// ⚠ 这里不能直接 CloseHandle 就走：那个 mutex 是别人的，关掉自己这个
			//   句柄不会影响它；但要注意 CreateMutexW 返回的是**同一个内核对象**的新句柄，
			//   我们已经把它的所有权（bInitialOwner=TRUE）拿到手了，必须主动释放一次，
			//   否则对方的 WaitForSingleObject 会被我们永久卡住。
			ReleaseMutex(h);
			CloseHandle(h);

			// 找到已有实例的窗口，把它叫到前面（用户体验：点第二次图标不再毫无反应）
			if (HWND existing = FindWindowW(L"ZDock", nullptr)) {
				SetForegroundWindow(existing);
			}
			log(L"[single] 已有实例在运行 -> 本次启动放弃");
			return false;
		}

		mutex = h;
		log(L"[single] 占住单实例 mutex");
		return true;
	}

} // namespace zdock
