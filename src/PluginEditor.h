#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "WorkerClient.h"
#include "Stems.h"
#include "SeparationEngine.h"

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
    void workerDied (const juce::String& reason) override;

    // SeparationEngine ChangeBroadcaster (message thread).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void timerCallback() override;

    void openSettingsDialog();
    void onSettingsSaved (const juce::String& pythonPath, const juce::String& modelPath);

    void refreshModelList();
    void populateModelCombo (const juce::Array<juce::var>& models);
    void onModelChanged();
    void requestModelInfo (const juce::String& modelName);

    void onStartButtonClicked();
    void updateStartButtonText();
    void updateProgressDisplay();

    SeparationParams gatherParams() const;

    PyMSSProcessorImpl& processor;
    PyMSSDocumentController* dc = nullptr;

    juce::Label titleLabel;
    juce::TextButton settingsButton { "Settings" };
    juce::Label modelLabel { {}, "Model" };
    juce::ComboBox modelCombo;
    juce::Label introLabel { {}, "Model Info" };
    juce::TextEditor introBox;

    juce::Label batchLabel { {}, "batch_size" };
    juce::Label overlapLabel { {}, "overlap_size" };
    juce::Label chunkLabel { {}, "chunk_size" };
    juce::TextEditor batchEdit, overlapEdit, chunkEdit;
    juce::ToggleButton normalizeToggle { "normalize" };
    juce::Label paramsHint { {}, "0 = use model default" };

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
    bool hasCachedModels = false;
    juce::String cachedPymssMessage;
    bool cachedPymssOk = false;
    bool hasPymssCheck = false;
    bool modelsNeedRefresh = false;
    bool pymssNeedsApply = false;
    int lastModelInfoTag = -1;

    juce::Component::SafePointer<juce::DialogWindow> settingsDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PyMSSAudioProcessorEditor)
};
