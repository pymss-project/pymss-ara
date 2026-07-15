#include "Settings.h"

SettingsStore::SettingsStore()
{
    auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    settingsDir = home.getChildFile (".pymss").getChildFile ("settings");
    settingsFile = settingsDir.getChildFile ("ara.json");
    logDir = home.getChildFile (".pymss").getChildFile ("logs");
    ensureDirectories();
}

juce::File SettingsStore::getSettingsFile() const { return settingsFile; }
juce::File SettingsStore::getSettingsDir()  const { return settingsDir; }
juce::File SettingsStore::getLogDir()       const { return logDir; }

void SettingsStore::ensureDirectories() const
{
    if (! settingsDir.exists())
        settingsDir.createDirectory();
    if (! logDir.exists())
        logDir.createDirectory();

    // Ensure an (empty) config file exists so users can hand-edit it.
    if (! settingsFile.exists())
    {
        auto empty = std::make_unique<juce::DynamicObject>();
        empty->setProperty ("python_path", "");
        empty->setProperty ("model_path", "");
        settingsFile.replaceWithText (juce::JSON::toString (juce::var (empty.release()), true));
    }
}

AraSettings SettingsStore::load() const
{
    ensureDirectories();

    AraSettings s;
    if (auto parsed = juce::JSON::parse (settingsFile); parsed.isObject())
    {
        s.pythonPath = parsed.getProperty ("python_path", "").toString();
        s.modelPath  = parsed.getProperty ("model_path", "").toString();
    }
    return s;
}

void SettingsStore::save (const AraSettings& settings) const
{
    ensureDirectories();

    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("python_path", settings.pythonPath);
    obj->setProperty ("model_path", settings.modelPath);
    settingsFile.replaceWithText (juce::JSON::toString (juce::var (obj.release()), true));
}
