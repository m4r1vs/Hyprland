#pragma once

#include "../defines.hpp"
#include "../protocols/ToplevelExport.hpp"
#include "../protocols/TextInputV1.hpp"
#include "../protocols/GlobalShortcuts.hpp"
#include "../protocols/Screencopy.hpp"
#include "../helpers/memory/WeakPtr.hpp"
#include "../helpers/signal/Listener.hpp"
#include <unordered_map>

class CProtocolManager {
  public:
    CProtocolManager();
    ~CProtocolManager();

    bool isGlobalPrivileged(const wl_global* global);

  private:
    std::unordered_map<std::string, CHyprSignalListener> m_mModeChangeListeners;

    void                                                 onMonitorModeChange(PHLMONITOR pMonitor);
};

inline std::unique_ptr<CProtocolManager> g_pProtocolManager;
