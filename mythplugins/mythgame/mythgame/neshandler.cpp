#include <string>
#include <zlib.h>
#include <qdir.h>
#include <qsqldatabase.h>
#include <mythtv/mythcontext.h>
#include "neshandler.h"
#include "nesrominfo.h"
#include "nessettingsdlg.h"

#include <iostream>

using namespace std;

NesHandler* NesHandler::pInstance = 0;

NesHandler* NesHandler::getHandler()
{
    if(!pInstance)
    {
        pInstance = new NesHandler();
    }
    return pInstance;
}

void NesHandler::start_game(RomInfo* romdata)
{
    QString exec = gContext->GetSetting("NesBinary") + " " +
        "'" + gContext->GetSetting("NesRomLocation") + "/" + romdata->Romname() + "'";
    cout << exec << endl;
    
    // Run the emulator and wait for it to terminate.      
    FILE* command = popen(exec, "w");
    pclose(command);
}

void NesHandler::edit_settings(QWidget* parent, RomInfo* romdata)
{
    // Eliminate unused parameter warning from the compiler.        
    parent = parent;
    romdata = romdata;
}

void NesHandler::edit_system_settings(QWidget* parent, RomInfo* romdata)
{
    // Eliminate unused parameter warning from the compiler.                
    romdata = romdata;

    NesSettingsDlg settingsdlg(parent, "gamesettings", true);
    settingsdlg.Show();
}

void NesHandler::processGames()
{
    QString thequery;

    QSqlDatabase* db = QSqlDatabase::database();

    // Remove all metadata entries from the tables, all correct values will be
    // added as they are found.  This is done so that entries that may no longer be
    // available or valid are removed each time the game list is remade.
    thequery = "DELETE FROM gamemetadata WHERE system = \"Nes\";";
    db->exec(thequery);

    // Search the rom dir for valid new roms.
    QDir RomDir(gContext->GetSetting("NesRomLocation"));
    const QFileInfoList* List = RomDir.entryInfoList();

    if (!List)
        return;

    for (QFileInfoListIterator it(*List); it; ++it)
    {
        QFileInfo Info(*it.current());
        if (IsNesRom(Info.filePath()))
        {
            QString GameName = GetGameName(Info.filePath());
            if (GameName.isNull())
            {
                GameName = Info.fileName();
            }
            cout << GameName << endl;

            QString Genre("Unknown");
            int Year = 0;
            GetMetadata(GameName, &Genre, &Year);
            
            // Put the game into the database.
            thequery = QString("INSERT INTO gamemetadata "
                               "(system, romname, gamename, genre, year) "
                               "VALUES (\"Nes\", \"%1\", \"%2\", \"%3\", %4);")
                               .arg(Info.fileName().latin1())
                               .arg(GameName.latin1()).arg(Genre.latin1())
                               .arg(Year);
            db->exec(thequery);
        }
        else
        {
            // Unknown type of file.
        }
    }
}

RomInfo* NesHandler::create_rominfo(RomInfo* parent)
{
    return new NesRomInfo(*parent);
}

bool NesHandler::IsNesRom(QString Path)
{
    bool NesRom = false;

    QFile f(Path);
    if (f.open(IO_ReadOnly)) 
    {
        // Use the magic number for iNes files to check against the file.
        const char Magic[] = "NES\032";
        char First4Bytes[4];
        f.readBlock(First4Bytes, 4);
        if (strncmp(Magic, First4Bytes, 4) == 0)
        {
            NesRom = true;
        }
        else
        {
            NesRom = false;
        }
        f.close();
    }
    return  NesRom;
}

QString NesHandler::GetGameName(QString Path)
{
    static bool bCRCMapLoaded = false;
    static map<QString, QString> CRCMap;

    // Load the CRC -> GoodName map if we haven't already.
    if (!bCRCMapLoaded) 
    {
        LoadCRCFile(CRCMap);
        bCRCMapLoaded = true;
    }

    // Try to get the GoodNES name for this file.
    QString GoodName;
    QFile f(Path);
    if (f.open(IO_ReadOnly))
    {
        // Get CRC of file
        char block[8192];
        Q_LONG count;
        uLong crc = crc32(0, Z_NULL, 0);

        // Skip past iNes header
        f.readBlock(block, 16);

        // Get CRC of rom data
        while ((count = f.readBlock(block, 8192))) 
        {
            crc = crc32(crc, (Bytef *)block, (uInt)count);
        }
        QString CRC;
        CRC.setNum(crc, 16);

        // Match CRC against crc file
        map<QString, QString>::iterator i;
        if ((i = CRCMap.find(CRC)) != CRCMap.end())
        {
            GoodName = i->second;
        }

        f.close();
    }

    return GoodName;
}

void NesHandler::LoadCRCFile(map<QString, QString> &CRCMap)
{
    QString CRCFilePath = gContext->GetSetting("NesCRCFile");
    QFile CRCFile(CRCFilePath);
    if (CRCFile.open(IO_ReadOnly)) 
    {
        QString line;
        while (CRCFile.readLine(line, 256) != -1) 
        {
            if (line[0] == '#') 
            {
                continue;
            }

            QStringList fields(QStringList::split("|", line));
            QStringList CRCName(QStringList::split("=", fields.first()));
            QString CRC(CRCName.first());
            CRCName.pop_front();
            QString Name(CRCName.first());
      
            if (!CRC.isNull() && !Name.isNull())
            {
                CRCMap[CRC] = Name.stripWhiteSpace();
            }
        }
        CRCFile.close();
    }
}

void NesHandler::GetMetadata(QString GameName, QString* Genre, int* Year)
{
    // Try to match the GoodNES name against the title table to get the metadata.
    
    QString thequery;
    thequery = QString("SELECT releasedate, keywords FROM nestitle "
                       "WHERE MATCH(description) AGAINST ('%1');")
                       .arg(GameName);
    QSqlDatabase* db = QSqlDatabase::database();
    QSqlQuery query = db->exec(thequery);
    if (query.isActive() && query.numRowsAffected() > 0)
    {
        // Take first entry since that will be the most relevant match.
        query.first();
        *Year = query.value(0).toInt();

        // To get the genre, use the first keyword that doesn't count the number
        // of players.
        QStringList keywords(QStringList::split(" ", query.value(1).toString()));
        for (QStringList::Iterator keyword = keywords.begin(); keyword != keywords.end(); ++keyword)
        {
            if ((*keyword)[0].isDigit())
                continue;

            thequery = QString("SELECT value FROM neskeyword WHERE keyword = '%1';").arg(*keyword);
            QSqlQuery query = db->exec(thequery);
            if (query.isActive() && query.numRowsAffected() > 0)
            {
                query.first();
                *Genre = query.value(0).toString();
                break;
            }
        }
    }
}
