#pragma once

#include <juce_core/juce_core.h>

/** Persistent plugin settings stored at ~/.pymss/settings/ara.json.

    Fields:
        python_path : interpreter that has pymss installed (empty => system "python")
        model_path  : directory containing downloaded model files (empty => pymss default)
*/
struct AraSettings
{
    juce::String pythonPath;
    juce::String modelPath;

    /** Resolve the python interpreter to launch. Falls back to "python". */
    juce::String resolvePythonExecutable() const
    {
        auto trimmed = pythonPath.trim();
        return trimmed.isNotEmpty() ? trimmed : "python";
    }
};

class SettingsStore
{
public:
    SettingsStore();

    /** ~/.pymss/settings/ara.json (created on first access). */
    juce::File getSettingsFile() const;
    juce::File getSettingsDir() const;
    juce::File getLogDir() const;

    AraSettings load() const;
    void save (const AraSettings& settings) const;

private:
    void ensureDirectories() const;

    juce::File settingsDir;
    juce::File settingsFile;
    juce::File logDir;
};
