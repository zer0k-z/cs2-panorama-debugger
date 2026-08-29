#include <windows.h>

#include <atomic>
#include <thread>

#include "ISmmPlugin.h"
#include "eiface.h"

#include "core/console.h"
#include "debugger.h"

// Declares g_SMAPI / g_PLAPI / g_PLID / g_SHPtr, which PLUGIN_SAVEVARS fills in.
PLUGIN_GLOBALVARS();

namespace
{

// panorama.dll, panoramauiclient.dll and the UI engine publish at different
// points during startup, and the plugin can load before or after any of them, so
// retry until everything resolves. Nothing here touches the game beyond reading
// module state; the frame hook puts the rest on the game thread.
class Bootstrap
{
public:
	void Start()
	{
		m_stop.store(false, std::memory_order_relaxed);
		m_thread = std::thread([this] { Run(); });
	}

	void Stop()
	{
		m_stop.store(true, std::memory_order_relaxed);
		if (m_thread.joinable())
			m_thread.join();
	}

private:
	void Run()
	{
		// Up to a couple of minutes: loading before the game has finished
		// starting is normal, and giving up early would look like a dead key.
		for (int attempt = 0; attempt < 600; ++attempt)
		{
			if (m_stop.load(std::memory_order_relaxed))
				return;
			if (panodbg::Initialize())
				return;

			Sleep(200);
		}

		panodbg::Console::Warnf("gave up waiting for the panorama UI engine");
	}

	std::thread m_thread;
	std::atomic<bool> m_stop{false};
};

Bootstrap g_bootstrap;

IVEngineServer2* g_engineServer = nullptr;

// A plain function so debugger.cpp needs no SDK type. ServerCommand is a pure
// virtual call and costs no link dependency; the convar API proper
// (ConVarRefAbstract) is out-of-line in tier1 and would drag it in for two
// settings.
void RunConsoleCommand(const char* command)
{
	if (g_engineServer)
		g_engineServer->ServerCommand(command);
}

} // namespace

class PanoramaDebuggerPlugin final : public ISmmPlugin
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override
	{
		PLUGIN_SAVEVARS();

		g_engineServer = static_cast<IVEngineServer2*>(
			ismm->VInterfaceMatch(ismm->GetEngineFactory(), INTERFACEVERSION_VENGINESERVER));
		if (!g_engineServer)
			panodbg::Console::Warnf("no " INTERFACEVERSION_VENGINESERVER
								   " -- the render convars will not be set");
		panodbg::SetConsoleCommandSink(&RunConsoleCommand);

		panodbg::Console::Printf("loading");
		g_bootstrap.Start();
		return true;
	}

	bool Unload(char* error, size_t maxlen) override
	{
		// Stop the retry loop first, so it cannot install a hook while we are
		// removing them. A vtable entry left pointing into this image after the
		// DLL unloads crashes the next time the game calls through it.
		g_bootstrap.Stop();
		panodbg::Shutdown();
		panodbg::Console::Printf("unloaded");
		return true;
	}

	const char* GetAuthor() override { return "zer0.k"; }
	const char* GetName() override { return "CS2 Panorama Debugger"; }
	const char* GetDescription() override { return "Toggles the native Panorama debugger with F6"; }
	const char* GetURL() override { return ""; }
	const char* GetLicense() override { return "MIT"; }
	const char* GetVersion() override { return "1.0.0"; }
	const char* GetDate() override { return __DATE__; }
	const char* GetLogTag() override { return "PANODBG"; }
};

PanoramaDebuggerPlugin g_plugin;
PLUGIN_EXPOSE(PanoramaDebuggerPlugin, g_plugin);
