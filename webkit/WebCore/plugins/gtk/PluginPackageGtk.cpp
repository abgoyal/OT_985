

#include "config.h"
#include "PluginPackage.h"

#include <stdio.h>

#include "CString.h"
#include "MIMETypeRegistry.h"
#include "NotImplemented.h"
#include "npruntime_impl.h"
#include "PluginDebug.h"

namespace WebCore {

bool PluginPackage::fetchInfo()
{
#if defined(XP_UNIX)
    if (!load())
        return false;

    NP_GetMIMEDescriptionFuncPtr NP_GetMIMEDescription = 0;
    NPP_GetValueProcPtr NPP_GetValue = 0;

    g_module_symbol(m_module, "NP_GetMIMEDescription", (void**)&NP_GetMIMEDescription);
    g_module_symbol(m_module, "NP_GetValue", (void**)&NPP_GetValue);

    if (!NP_GetMIMEDescription || !NPP_GetValue)
        return false;

    char* buffer = 0;
    NPError err = NPP_GetValue(0, NPPVpluginNameString, &buffer);
    if (err == NPERR_NO_ERROR)
        m_name = buffer;

    buffer = 0;
    err = NPP_GetValue(0, NPPVpluginDescriptionString, &buffer);
    if (err == NPERR_NO_ERROR) {
        m_description = buffer;
        determineModuleVersionFromDescription();
    }

    const gchar* types = NP_GetMIMEDescription();
    gchar** mimeDescs = g_strsplit(types, ";", -1);
    for (int i = 0; mimeDescs[i] && mimeDescs[i][0]; i++) {
        gchar** mimeData = g_strsplit(mimeDescs[i], ":", 3);
        if (g_strv_length(mimeData) < 3) {
            g_strfreev(mimeData);
            continue;
        }

        String description = String::fromUTF8(mimeData[2]);
        gchar** extensions = g_strsplit(mimeData[1], ",", -1);

        Vector<String> extVector;
        for (int j = 0; extensions[j]; j++)
            extVector.append(String::fromUTF8(extensions[j]));

        determineQuirks(mimeData[0]);
        m_mimeToExtensions.add(mimeData[0], extVector);
        m_mimeToDescriptions.add(mimeData[0], description);

        g_strfreev(extensions);
        g_strfreev(mimeData);
    }
    g_strfreev(mimeDescs);

    return true;
#else
    notImplemented();
    return false;
#endif
}

bool PluginPackage::load()
{
    if (m_isLoaded) {
        m_loadCount++;
        return true;
    }

    m_module = g_module_open((m_path.utf8()).data(), G_MODULE_BIND_LOCAL);

    if (!m_module) {
        LOG(Plugins,"Module Load Failed :%s, Error:%s\n", (m_path.utf8()).data(), g_module_error());
        return false;
    }

    m_isLoaded = true;

    NP_InitializeFuncPtr NP_Initialize = 0;
    m_NPP_Shutdown = 0;

    NPError npErr;

    g_module_symbol(m_module, "NP_Initialize", (void**)&NP_Initialize);
    g_module_symbol(m_module, "NP_Shutdown", (void**)&m_NPP_Shutdown);

    if (!NP_Initialize || !m_NPP_Shutdown)
        goto abort;

    memset(&m_pluginFuncs, 0, sizeof(m_pluginFuncs));
    m_pluginFuncs.size = sizeof(m_pluginFuncs);

    initializeBrowserFuncs();

#if defined(XP_UNIX)
    npErr = NP_Initialize(&m_browserFuncs, &m_pluginFuncs);
#else
    npErr = NP_Initialize(&m_browserFuncs);
#endif
    if (npErr != NPERR_NO_ERROR)
        goto abort;

    m_loadCount++;
    return true;

abort:
    unloadWithoutShutdown();
    return false;
}

}
