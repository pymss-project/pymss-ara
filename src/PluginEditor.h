#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

#include "PluginProcessor.h"
#include "ipc/WorkerClient.h"
#include "core/Stems.h"
#include "core/SeparationEngine.h"

/** One row in the stem monitor: name + Mute + Solo. */
class StemMixRow : public juce::Component
{
public:
    StemMixRow (int stemIndex, const juce::String& name, SeparationEngine& engine);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int index;
    SeparationEngine& engine;
    juce::Label nameLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemMixRow)
};

class PyMSSAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  public juce::AudioProcessorEditorARAExtension,
                                  public WorkerClient::Listener,
                                  public juce::ChangeListener,
                                  private juce::Timer
{
public:
    explicit PyMSSAudioProcessorEditor (PyMSSProcessorImpl& p);
    ~PyMSSAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // WorkerClient::Listener (worker reader thread).
    void workerReady (bool pymssOk, const juce::String& version, const juce::String& message) override;
    void pymssCheckResult (int tag, bool ok, const juce::String& version, const juce::String& message) override;
    void modelListResult (int tag, const juce::Array<juce::var>& models) override;
    void modelInfoResult (int tag, const juce::var& info) override;
    void modelInfoFailed (int tag, const juce::String& message) override;
    void modelDownloadProgress (int tag, juce::int64 done, juce::int64 total,
                                const juce::String& message) override;
    void modelDownloadDone (int tag, const juce::String& modelName, const juce::var& info) override;
    void modelDownloadFailed (int tag, const juce::String& message) override;
    void workerDied (const juce::String& reason) override;

    // SeparationEngine ChangeBroadcaster (message thread).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void timerCallback() override;

    void openSettingsDialog();
    void onSettingsSaved (const juce::String& pythonPath, const juce::String& modelPath);
    void openModelDownloadDialog();
    void startModelDownload (const juce::String& modelName);

    void refreshModelList();
    void populateModelCombo (const juce::Array<juce::var>& models);
    void onModelChanged (bool applyDefaults = true);
    void requestModelInfo (const juce::String& modelName, bool applyDefaults = true);
    void applyModelInfo (const juce::var& info, bool applyDefaults = true);
    void setInferenceParams (const SeparationParams& params);
    void updateParameterControls (bool isVrModel);
    bool isModelInstalled (const juce::String& modelName);

    void onStartButtonClicked();
    void updateStartButtonText();
    void updateProgressDisplay();

    SeparationParams gatherParams() const;

    PyMSSProcessorImpl& processor;
    PyMSSDocumentController* dc = nullptr;

    juce::Label titleLabel;
    juce::TextButton modelsButton { "Models..." };
    juce::TextButton settingsButton { "Settings" };
    juce::Label modelLabel { {}, "Model" };
    juce::ComboBox modelCombo;
    juce::Label introLabel { {}, "Model Info" };
    juce::TextEditor introBox;

    juce::Label batchLabel { {}, "batch_size" };
    juce::Label overlapLabel { {}, "overlap_size" };
    juce::Label chunkLabel { {}, "chunk_size" };
    juce::Label windowLabel { {}, "window_size" };
    juce::Label aggressionLabel { {}, "aggression" };
    juce::Label postProcessThresholdLabel { {}, "post_threshold" };
    juce::TextEditor batchEdit, overlapEdit, chunkEdit, windowEdit, aggressionEdit, postProcessThresholdEdit;
    juce::ToggleButton enableTtaToggle { "enable_tta" };
    juce::ToggleButton standardizeToggle { "standardize" };
    juce::ToggleButton highEndProcessToggle { "high_end_process" };
    juce::ToggleButton enablePostProcessToggle { "post_process" };
    juce::ToggleButton normalizeToggle { "normalize" };
    juce::Label paramsHint { {}, "0 = use model default" };
    bool currentModelIsVr = false;

    juce::TextButton startButton { "Start Separation" };
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Label statusLabel;

    juce::Label stemsLabel { {}, "Stems" };
    juce::OwnedArray<StemMixRow> stemRows;
    juce::String lastBuiltStemSourceId;
    void rebuildStemRows();

    // Cached worker results (guarded by lock; applied on the message thread).
    juce::CriticalSection cacheLock;
    juce::Array<juce::var> cachedModels;
    std::atomic<bool> hasCachedModels { false };
    juce::String cachedPymssMessage;
    bool cachedPymssOk = false;
    bool hasPymssCheck = false;
    bool modelsNeedRefresh = false;
    bool pymssNeedsApply = false;
    std::atomic<int> lastModelInfoTag { -1 };
    juce::var cachedModelInfo;
    juce::String cachedModelInfoError;
    int cachedModelInfoTag = -1;
    bool modelInfoNeedsApply = false;
    bool modelInfoLoading = false; // message thread only
    bool modelInfoShouldApplyDefaults = true; // message thread only
    bool preserveParamsOnInitialSelection = false; // message thread only

    std::atomic<int> activeDownloadTag { -1 };
    bool downloadBusy = false;
    juce::int64 cachedDownloadDone = 0;
    juce::int64 cachedDownloadTotal = 0;
    juce::String cachedDownloadMessage;
    juce::String cachedDownloadModel;
    juce::String cachedDownloadError;
    juce::var cachedDownloadInfo;
    bool downloadProgressNeedsApply = false;
    bool downloadFinishedNeedsApply = false;
    bool cachedDownloadSucceeded = false;
    juce::String pendingDefaultsRefreshModel;
    bool modelSelectionInitialized = false;

    juce::Component::SafePointer<juce::DialogWindow> settingsDialog;
    juce::Component::SafePointer<juce::DialogWindow> modelDownloadDialog;
    juce::Component::SafePointer<juce::Component> modelDownloadPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PyMSSAudioProcessorEditor)
};
