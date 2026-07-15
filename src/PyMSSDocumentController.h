#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Settings.h"
#include "WorkerClient.h"
#include "SeparationEngine.h"
#include "PyMSSPlaybackRenderer.h"

/** Central document controller specialisation.

    Owns the shared plugin machinery (settings store, Python worker client and
    separation engine) and creates the playback renderer. Both the AudioProcessor
    and the Editor reach the engine/worker through this object via
    ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PyMSSDocumentController>().
*/
class PyMSSDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    PyMSSDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                             const ARA::ARADocumentControllerHostInstance* instance)
        : juce::ARADocumentControllerSpecialisation (entry, instance) {}

    // Accessors used by the processor / editor.
    SettingsStore&       getSettingsStore() { return settingsStore; }
    WorkerClient&        getWorker()        { return worker; }
    SeparationEngine&    getEngine()        { return engine; }

    /** Read an entire ARA audio source into a channel-major buffer at its
        native sample rate. Returns false on failure. Safe to call off the
        message thread (used by the separation engine's background thread). */
    bool readAudioSource (juce::ARAAudioSource* source,
                          juce::AudioBuffer<float>& out,
                          double& nativeSampleRate);

    //----------------------------------------------------------------------
    // Worker lifecycle.
    /** Launch (or re-launch) the Python worker using the current settings. */
    void startWorker();
    void stopWorker();
    bool isWorkerRunning() const { return worker.isRunning(); }

    /** Convenience: load current settings. */
    AraSettings loadSettings() const { return settingsStore.load(); }
    void saveSettings (const AraSettings& s) { settingsStore.save (s); }

protected:
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override
    {
        return new PyMSSPlaybackRenderer (getDocumentController(), engine);
    }

    // The plugin keeps no per-source persistent state, so archiving is a no-op.
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                     const juce::ARARestoreObjectsFilter* filter) noexcept override;
    bool doStoreObjectsToStream (juce::ARAOutputStream& output,
                                 const juce::ARAStoreObjectsFilter* filter) noexcept override;

private:
    SettingsStore settingsStore;
    WorkerClient worker;
    SeparationEngine engine { worker };
};
