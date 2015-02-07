

#include "config.h"
#include "ContextMenuItem.h"

#include "ContextMenu.h"
#include "NotImplemented.h"

#include <Menu.h>
#include <MenuItem.h>
#include <Message.h>
#include <String.h>


using namespace WebCore;

ContextMenuItem::ContextMenuItem(PlatformMenuItemDescription item)
{
    m_platformDescription = item;
}

ContextMenuItem::ContextMenuItem(ContextMenu* subMenu)
{
    m_platformDescription = new BMenuItem(subMenu->platformDescription(),
                                          new BMessage(ContextMenuItemTagNoAction));
}

ContextMenuItem::ContextMenuItem(ContextMenuItemType type, ContextMenuAction action,
                                 const String& title, ContextMenu* subMenu)
{
    if (type == ActionType)
        m_platformDescription = new BMenuItem(BString(title).String(), new BMessage(action));
    else if (type == SeparatorType)
        m_platformDescription = new BSeparatorItem();
    else {
        m_platformDescription = new BMenuItem(subMenu->platformDescription(), new BMessage(action));
        m_platformDescription->SetLabel(BString(title).String());
    }
}

ContextMenuItem::~ContextMenuItem()
{
    delete m_platformDescription;
}

PlatformMenuItemDescription ContextMenuItem::releasePlatformDescription()
{
    BMenuItem* item = m_platformDescription;
    m_platformDescription = 0;
    return item;
}

ContextMenuItemType ContextMenuItem::type() const
{
    if (dynamic_cast<BSeparatorItem*>(m_platformDescription))
        return SeparatorType;
    if (m_platformDescription->Submenu())
        return SubmenuType;
    return ActionType;
}

void ContextMenuItem::setType(ContextMenuItemType type)
{
    ContextMenuAction theAction = action();
    String theTitle = title();
    BMenu* subMenu = platformSubMenu();
    delete m_platformDescription;

    if (type == ActionType)
        m_platformDescription = new BMenuItem(BString(theTitle).String(), new BMessage(theAction));
    else if (type == SeparatorType)
        m_platformDescription = new BSeparatorItem();
    else {
        if (subMenu) {
            m_platformDescription = new BMenuItem(subMenu, new BMessage(theAction));
            m_platformDescription->SetLabel(BString(theTitle).String());
        } else
            m_platformDescription = new BMenuItem(BString(theTitle).String(), new BMessage(theAction));
    }
}

ContextMenuAction ContextMenuItem::action() const
{
    if (!m_platformDescription)
        return ContextMenuItemTagNoAction;
    return static_cast<WebCore::ContextMenuAction>(m_platformDescription->Message()->what);
}

void ContextMenuItem::setAction(ContextMenuAction action)
{
    if (m_platformDescription)
        m_platformDescription->Message()->what = action;
}

String ContextMenuItem::title() const
{
    if (m_platformDescription)
        return "";
    return BString(m_platformDescription->Label());
}

void ContextMenuItem::setTitle(const String& title)
{
    // FIXME: We need to find a better way to convert WebKit Strings into c strings
    m_platformDescription->SetLabel(BString(title).String());
}

PlatformMenuDescription ContextMenuItem::platformSubMenu() const
{
    return m_platformDescription->Submenu();
}

void ContextMenuItem::setSubMenu(ContextMenu* menu)
{
    // FIXME: We assume m_platformDescription is valid
    const char* title = m_platformDescription->Label();
    delete m_platformDescription;
    m_platformDescription = new BMenuItem(menu->platformDescription(), new BMessage(action()));
    m_platformDescription->SetLabel(title);
}

void ContextMenuItem::setChecked(bool checked)
{
    if (m_platformDescription)
        m_platformDescription->SetMarked(checked);
}

void ContextMenuItem::setEnabled(bool enable)
{
    if (m_platformDescription)
        m_platformDescription->SetEnabled(enable);
}

bool ContextMenuItem::enabled() const
{
    if (!m_platformDescription)
        return true;
    return m_platformDescription->IsEnabled();
}

