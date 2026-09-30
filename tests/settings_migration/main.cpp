#include "settings/settingsmigration.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdlib>

static void require(bool condition, const char* message)
{
    if (!condition) qFatal("%s", message);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "Temporary settings directory unavailable");
    QSettings previous(directory.filePath("previous.ini"), QSettings::IniFormat);
    QSettings destination(directory.filePath("destination.ini"), QSettings::IniFormat);
    require(!migrateSettingsIfEmpty(destination, previous), "Empty profile should not migrate");
    require(!loadDualSenseSpeakerAudio(destination, true), "New speaker output must be opt-in");
    previous.setValue("dualSenseHapticsMode", 0);
    previous.setValue("hosts/example/certificate", QByteArray("synthetic-test-certificate"));
    previous.sync();
    require(migrateSettingsIfEmpty(destination, previous), "Legacy settings should migrate");
    require(destination.value("dualSenseHapticsMode").toInt() == 0, "Native preference lost");
    require(loadDualSenseSpeakerAudio(destination, true), "Legacy physical speaker behavior lost");
    destination.setValue("dualSenseSpeakerAudio", false);
    require(!loadDualSenseSpeakerAudio(destination, true), "Explicit speaker mute was ignored");
    destination.setValue("dualSenseSpeakerAudio", true);
    require(loadDualSenseSpeakerAudio(destination, true), "Explicit speaker opt-in was ignored");
    destination.remove("dualSenseSpeakerAudio");
    require(!loadDualSenseSpeakerAudio(destination, false), "Emulated profile opted into speaker audio");
    require(destination.value("hosts/example/certificate").toByteArray() ==
            QByteArray("synthetic-test-certificate"), "Pairing data lost");
    destination.setValue("dualSenseHapticsMode", 1);
    require(!migrateSettingsIfEmpty(destination, previous), "Existing profile must not be overwritten");
    require(destination.value("dualSenseHapticsMode").toInt() == 1, "Existing preference overwritten");
    QSettings reloaded(directory.filePath("destination.ini"), QSettings::IniFormat);
    destination.sync();
    reloaded.sync();
    require(reloaded.contains("hosts/example/certificate"), "Migration did not persist");
    qInfo("Settings migration tests passed");
    return EXIT_SUCCESS;
}
