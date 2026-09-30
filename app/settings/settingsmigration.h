#pragma once

#include <QSettings>

inline bool loadDualSenseSpeakerAudio(const QSettings& settings, bool physicalMode)
{
    return settings.value("dualSenseSpeakerAudio",
        settings.contains("dualSenseHapticsMode") && physicalMode).toBool();
}

// Import a previous application namespace only into an untouched profile.
// Existing destination preferences and pairing data always take precedence.
inline bool migrateSettingsIfEmpty(QSettings& destination, const QSettings& previous)
{
    if (!destination.allKeys().isEmpty() || previous.allKeys().isEmpty()) {
        return false;
    }
    for (const QString& key : previous.allKeys()) {
        destination.setValue(key, previous.value(key));
    }
    destination.sync();
    return destination.status() == QSettings::NoError;
}
