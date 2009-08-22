
#include <unistd.h>

// QT headers
#include <QApplication>
#include <QDir>

// MythTV headers
#include <lcddevice.h>
#include <mythcontext.h>
#include <mythplugin.h>
#include <mythpluginapi.h>
#include <mythversion.h>
#include <myththemedmenu.h>
#include <mythmainwindow.h>
#include <mythuihelper.h>

// MythWeather headers
#include "weather.h"
#include "weatherSetup.h"
#include "sourceManager.h"
#include "dbcheck.h"

SourceManager *srcMan = 0;

void runWeather();
int  RunWeather();

void setupKeys()
{
    REG_JUMP("MythWeather", "Weather forecasts", "", runWeather);
    REG_KEY("Weather", "PAUSE", "Pause current page", "P");
    REG_KEY("Weather", "SEARCH", "Search List", "/");
    REG_KEY("Weather", "NEXTSEARCH", "Search List", "n");
    REG_KEY("Weather", "UPDATE", "Search List", "u");
}

int mythplugin_init(const char *libversion)
{
    if (!gContext->TestPopupVersion("mythweather", libversion,
                                    MYTH_BINARY_VERSION))
        return -1;

    gContext->ActivateSettingsCache(false);
    InitializeDatabase();
    gContext->ActivateSettingsCache(true);

    setupKeys();

    if (gContext->GetNumSetting("weatherbackgroundfetch", 0))
    {
        srcMan = new SourceManager();
        srcMan->startTimers();
        srcMan->doUpdate();
    }

    return 0;
}

void runWeather()
{
    RunWeather();
}

int RunWeather()
{
    MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();

    Weather *weather = new Weather(mainStack, "mythweather", srcMan);

    if (weather->Create())
    {
        mainStack->AddScreen(weather);
        weather->setupScreens();
        return 0;
    }
    else
    {
        delete weather;
        return -1;
    }
}

int mythplugin_run()
{
    return RunWeather();
}

void WeatherCallback(void *data, QString &selection)
{
    (void) data;

    MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();

    if (selection == "SETTINGS_GENERAL")
    {
        GlobalSetup *gsetup = new GlobalSetup(mainStack, "weatherglobalsetup");

        if (gsetup->Create())
            mainStack->AddScreen(gsetup);
        else
            delete gsetup;
    }
    else if (selection == "SETTINGS_SCREEN")
    {
        ScreenSetup *ssetup = new ScreenSetup(mainStack, "weatherscreensetup", srcMan);

        if (ssetup->Create())
            mainStack->AddScreen(ssetup);
        else
            delete ssetup;
    }
    else if (selection == "SETTINGS_SOURCE")
    {
        SourceSetup *srcsetup = new SourceSetup(mainStack, "weathersourcesetup");

        if (srcsetup->Create())
            mainStack->AddScreen(srcsetup);
        else
            delete srcsetup;
//             MythPopupBox::showOkPopup(gContext->GetMainWindow(), "no sources",
//                     QObject::tr("No Sources defined, Sources are defined by"
//                                 " adding screens in Screen Setup."));
    }
}

int mythplugin_config()
{
    QString menuname = "weather_settings.xml";
    QString themedir = GetMythUI()->GetThemeDir();

    MythThemedMenu *menu = new MythThemedMenu(
        themedir, menuname,
        gContext->GetMainWindow()->GetMainStack(), "weather menu");

    menu->setCallback(WeatherCallback, 0);
    menu->setKillable();
    if (menu->foundTheme())
    {
        if (LCD *lcd = LCD::Get())
            lcd->switchToTime();

        GetMythMainWindow()->GetMainStack()->AddScreen(menu);
        return 0;
    }
    else
    {
        VERBOSE(VB_IMPORTANT, QString("Couldn't find menu %1 or theme %2")
                              .arg(menuname).arg(themedir));
        delete menu;
        return -1;
    }
}

void  mythplugin_destroy()
{
    if (srcMan)
    {
        delete srcMan;
        srcMan = 0;
    }
}
