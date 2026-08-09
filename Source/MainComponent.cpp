#include "MainComponent.h"

#include <cmath>

namespace
{
constexpr auto background = 0xff090b12;
constexpr auto panel = 0xff121725;
constexpr auto paleText = 0xffdce6e8;
constexpr auto quietText = 0xff7f9298;

bool looksLikeModel12(const juce::String& name)
{
    return name.containsIgnoreCase("model 12")
        || name.containsIgnoreCase("model12");
}

int highestConfiguredInput(const Model12AudioRouter::RoutingConfig& routing)
{
    return juce::jmax(routing.saxInputLeft, routing.saxInputRight);
}

int highestConfiguredOutput(const Model12AudioRouter::RoutingConfig& routing)
{
    return juce::jmax(
        juce::jmax(routing.ambientOutputLeft, routing.ambientOutputRight,
                   routing.bassOutputLeft, routing.bassOutputRight),
        juce::jmax(routing.saxOutputLeft, routing.saxOutputRight));
}

bool hasContiguousChannels(const juce::BigInteger& channels, int count)
{
    for (int channel = 0; channel < count; ++channel)
        if (! channels[channel])
            return false;
    return true;
}

const std::array<juce::Colour, EcosystemEngine::memoryCount> memoryColours {
    juce::Colour(0xff69d2e7), juce::Colour(0xffa7dbd8),
    juce::Colour(0xffe0e4cc), juce::Colour(0xfff38630),
    juce::Colour(0xffff4e50)
};

constexpr std::array<const char*, EcosystemEngine::memoryCount>
    performanceLevelSettingKeys {
        "levelBassMidi5", "levelPartMidi2", "levelPartMidi3",
        "levelPartMidi4", "levelSax"
    };

constexpr std::array<const char*, EcosystemEngine::memoryCount>
    delayLevelSettingKeys {
        "delayBassMidi5", "delayPartMidi2", "delayPartMidi3",
        "delayPartMidi4", "delaySax"
    };

constexpr std::array<float, EcosystemEngine::memoryCount> defaultDelayLevels {
    0.0f, 0.48f, 0.42f, 0.52f, 0.40f
};

constexpr auto minimumPerformanceLevelDb = -48.0;
constexpr auto defaultPerformanceLevelDb = -6.0;

juce::String performanceLevelText(float gain)
{
    const auto db = juce::Decibels::gainToDecibels(
        static_cast<double>(gain), minimumPerformanceLevelDb);
    return db <= minimumPerformanceLevelDb + 0.05
        ? juce::String("MUTO")
        : juce::String(db, 1) + " dB";
}

juce::String delayLevelText(float amount)
{
    return amount <= 0.005f
        ? juce::String("SENZA ECO")
        : juce::String("ECO ")
            + juce::String(static_cast<int>(std::round(amount * 100.0f))) + "%";
}
}

class MemoryOrb final : public juce::Component
{
public:
    MemoryOrb(int memoryIndex, juce::String memoryName, juce::Colour memoryColour,
              EcosystemEngine& ecosystem)
        : index(memoryIndex), name(std::move(memoryName)), colour(memoryColour), engine(ecosystem)
    {
        setRepaintsOnMouseActivity(true);
    }

    std::function<void(int)> onSelected;
    bool selected = false;
    double animation = 0.0;

    void paint(juce::Graphics& graphics) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(5.0f);
        const auto phase = engine.getPhase(index);
        const auto recording = engine.isRecording(index);
        const auto material = engine.hasMaterial(index);
        const auto isBass = EcosystemEngine::isLiveBassLayer(index);
        const auto bassActive = isBass && engine.isBassEnabled();
        const auto breathing = 1.0f + 0.035f * std::sin(static_cast<float>(animation * 2.0
                                                    + static_cast<double>(index)));

        juce::ColourGradient surface(
            colour.withAlpha(recording ? 0.26f
                : (material || bassActive ? 0.14f : 0.06f)),
            bounds.getX(), bounds.getY(), juce::Colour(panel).darker(0.32f),
            bounds.getRight(), bounds.getBottom(), false);
        graphics.setGradientFill(surface);
        graphics.fillRoundedRectangle(bounds, 24.0f);
        graphics.setColour(recording ? juce::Colours::white.withAlpha(0.92f)
            : colour.withAlpha(selected || bassActive ? 0.95f : 0.34f));
        graphics.drawRoundedRectangle(bounds.reduced(selected ? 3.0f : 2.0f), 22.0f,
                                      selected ? 6.0f : 2.5f);

        const auto isSax = index == EcosystemEngine::midiMemoryCount;
        auto phaseArea = isSax
            ? bounds.removeFromLeft(juce::jmin(260.0f, bounds.getHeight()))
            : bounds.withTrimmedTop(54.0f).withTrimmedBottom(86.0f);
        const auto centre = phaseArea.getCentre();
        const auto radius = 0.38f * juce::jmin(phaseArea.getWidth(), phaseArea.getHeight());

        juce::ColourGradient glow(colour.withAlpha(recording ? 0.52f : 0.24f),
                                  centre.x, centre.y, colour.withAlpha(0.0f),
                                  centre.x + radius * 1.35f, centre.y, true);
        graphics.setGradientFill(glow);
        graphics.fillEllipse(juce::Rectangle<float>(radius * 2.0f * breathing,
                                                     radius * 2.0f * breathing)
                                 .withCentre(centre));
        graphics.setColour(colour.withAlpha(
            material || recording || bassActive ? 0.70f : 0.20f));
        graphics.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f)
                                 .withCentre(centre), selected ? 5.0f : 3.0f);

        if (material || recording)
        {
            juce::Path arc;
            arc.addCentredArc(centre.x, centre.y, radius * 0.88f, radius * 0.88f,
                              0.0f, 0.0f,
                              static_cast<float>(juce::MathConstants<double>::twoPi
                                                 * juce::jlimit(0.0, 1.0, phase)), true);
            graphics.setColour(recording ? juce::Colours::white : colour.brighter(0.35f));
            graphics.strokePath(arc, juce::PathStrokeType(selected ? 10.0f : 7.0f,
                                juce::PathStrokeType::curved,
                                juce::PathStrokeType::rounded));
        }

        auto textArea = isSax ? bounds.reduced(20.0f, 14.0f)
                              : bounds.reduced(18.0f, 14.0f);
        graphics.setColour(juce::Colour(paleText));
        graphics.setFont(juce::FontOptions(selected ? 29.0f : 25.0f,
                                           juce::Font::bold));
        graphics.drawText(name, textArea.removeFromTop(42.0f),
                          isSax ? juce::Justification::centredLeft
                                : juce::Justification::centred, false);

        juce::String detail;
        if (isBass)
            detail = engine.isBassEnabled()
                ? "ATTIVO / MIDI 5 -> USCITA CONFIGURATA"
                : "MUTO / TOCCA RIATTIVA BASSO LIVE";
        else if (recording && material)
            detail = "NUTRE  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (recording)
            detail = "REGISTRA  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (material)
            detail = "SUONA  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else
            detail = index < EcosystemEngine::midiMemoryCount
                ? "VUOTA  /  MIDI " + juce::String(engine.getMidiChannelForMemory(index))
                : "VUOTA  /  INGRESSO CONFIGURATO";

        graphics.setColour(recording ? juce::Colours::white : juce::Colour(quietText));
        graphics.setFont(juce::FontOptions(isSax ? 24.0f : (isBass ? 16.0f : 19.0f),
                                           juce::Font::plain));
        graphics.drawText(detail, textArea.removeFromTop(isSax ? 44.0f : 34.0f),
                          isSax ? juce::Justification::centredLeft
                                : juce::Justification::centred, false);

        const auto& scenario = CommentoScenarios::get(engine.getScenarioIndex());
        auto timbre = isSax
            ? juce::String("SAX: ") + scenario.sax.name
            : juce::String(scenario.layers[static_cast<size_t>(index)].name);
        timbre += "  ·  " + performanceLevelText(
            engine.getPerformanceLevel(index));
        if (! isBass)
            timbre += "  ·  " + delayLevelText(engine.getDelayLevel(index));
        graphics.setColour(colour.withAlpha(0.76f));
        graphics.setFont(juce::FontOptions(isSax ? 18.0f : 16.0f,
                                           juce::Font::bold));
        graphics.drawText(timbre, textArea.removeFromTop(28.0f),
                          isSax ? juce::Justification::centredLeft
                                : juce::Justification::centred, false);

        if (isSax)
        {
            textArea.removeFromTop(18.0f);
            graphics.setColour(juce::Colour(quietText));
            graphics.setFont(juce::FontOptions(18.0f));
            graphics.drawText(engine.isSaxStereoInput()
                                  ? "STEREO / coppia configurata"
                                  : "MONO / canale sinistro al centro",
                              textArea.removeFromTop(30.0f),
                              juce::Justification::centredLeft, false);

            auto meter = textArea.removeFromBottom(30.0f).reduced(0.0f, 8.0f);
            graphics.setColour(juce::Colour(0xff202c36));
            graphics.fillRoundedRectangle(meter, 7.0f);
            const auto level = juce::jlimit(0.0f, 1.0f, engine.getSaxInputLevel());
            graphics.setColour(colour.withAlpha(0.88f));
            graphics.fillRoundedRectangle(
                meter.withWidth(meter.getWidth() * std::sqrt(level)), 7.0f);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onSelected)
            onSelected(index);
    }

private:
    int index;
    juce::String name;
    juce::Colour colour;
    EcosystemEngine& engine;
};

class ConnectionChoice final : public juce::Component
{
public:
    explicit ConnectionChoice(juce::String choiceTitle)
    {
        title.setText(std::move(choiceTitle), juce::dontSendNotification);
        title.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colour(quietText));
        title.setJustificationType(juce::Justification::centred);
        value.setFont(juce::FontOptions(19.0f, juce::Font::bold));
        value.setColour(juce::Label::textColourId, juce::Colour(paleText));
        value.setJustificationType(juce::Justification::centred);
        value.setMinimumHorizontalScale(0.55f);

        for (auto* button : { &previous, &next })
        {
            button->setColour(juce::TextButton::buttonColourId,
                              juce::Colour(0xff263444));
            button->setColour(juce::TextButton::textColourOffId,
                              juce::Colour(paleText));
            addAndMakeVisible(*button);
        }
        addAndMakeVisible(title);
        addAndMakeVisible(value);

        previous.onClick = [this] { move(-1); };
        next.onClick = [this] { move(1); };
    }

    std::function<void(int)> onChange;

    void setOptions(const juce::StringArray& newOptions, int newIndex = 0)
    {
        options = newOptions;
        setSelectedIndex(newIndex, false);
    }

    void setSelectedIndex(int newIndex, bool notify)
    {
        selectedIndex = options.isEmpty()
            ? -1 : juce::jlimit(0, options.size() - 1, newIndex);
        value.setText(selectedIndex >= 0 ? options[selectedIndex] : "NESSUNA SCELTA",
                      juce::dontSendNotification);
        previous.setEnabled(options.size() > 1);
        next.setEnabled(options.size() > 1);
        if (notify && onChange)
            onChange(selectedIndex);
    }

    [[nodiscard]] int getSelectedIndex() const noexcept { return selectedIndex; }
    [[nodiscard]] juce::String getSelectedText() const
    {
        return selectedIndex >= 0 ? options[selectedIndex] : juce::String();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.setColour(juce::Colour(0xff18202d));
        graphics.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 13.0f);
        graphics.setColour(juce::Colour(0xff344659));
        graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f),
                                      13.0f, 1.5f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 5);
        title.setBounds(area.removeFromTop(24));
        const auto arrowWidth = juce::jlimit(48, 76, area.getHeight());
        previous.setBounds(area.removeFromLeft(arrowWidth).reduced(2));
        next.setBounds(area.removeFromRight(arrowWidth).reduced(2));
        value.setBounds(area.reduced(5, 0));
    }

private:
    void move(int delta)
    {
        if (options.isEmpty())
            return;
        const auto nextIndex = (selectedIndex + delta + options.size()) % options.size();
        setSelectedIndex(nextIndex, true);
    }

    juce::Label title;
    juce::Label value;
    juce::TextButton previous { "<" };
    juce::TextButton next { ">" };
    juce::StringArray options;
    int selectedIndex = -1;
};

MainComponent::MainComponent()
{
    setOpaque(true);

    juce::PropertiesFile::Options propertyOptions;
    propertyOptions.applicationName = "Commento";
    propertyOptions.filenameSuffix = "settings";
    propertyOptions.folderName = "Commento";
    propertyOptions.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters(propertyOptions);

    titleLabel.setText("COMMENTO", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(34.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(paleText));
    addAndMakeVisible(titleLabel);

    subtitleLabel.setText("tre memorie, un basso e un respiro",
                          juce::dontSendNotification);
    subtitleLabel.setFont(juce::FontOptions(15.0f));
    subtitleLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addAndMakeVisible(subtitleLabel);

    scenarioLabel.setJustificationType(juce::Justification::centred);
    scenarioLabel.setFont(juce::FontOptions(19.0f, juce::Font::bold));
    scenarioLabel.setColour(juce::Label::textColourId, juce::Colour(paleText));
    scenarioLabel.setColour(juce::Label::backgroundColourId, juce::Colour(panel));
    addAndMakeVisible(scenarioLabel);
    addAndMakeVisible(previousScenarioButton);
    addAndMakeVisible(nextScenarioButton);

    statusLabel.setJustificationType(juce::Justification::centredRight);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addAndMakeVisible(statusLabel);

    for (auto* label : { &audioStatusLabel, &midiStatusLabel })
    {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::FontOptions(18.0f, juce::Font::bold));
        label->setColour(juce::Label::textColourId, juce::Colour(paleText));
        label->setColour(juce::Label::backgroundColourId, juce::Colour(panel));
        addAndMakeVisible(*label);
    }

    const std::array<juce::String, EcosystemEngine::memoryCount> names {
        "I - BASSO LIVE", "II - MAREA", "III - RADICE", "IV - SCINTILLA", "RESPIRO"
    };
    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        orbs[static_cast<size_t>(index)] = std::make_unique<MemoryOrb>(
            index, names[static_cast<size_t>(index)],
            memoryColours[static_cast<size_t>(index)], engine);
        orbs[static_cast<size_t>(index)]->onSelected = [this](int selected)
        {
            selectMemory(selected);
        };
        addAndMakeVisible(*orbs[static_cast<size_t>(index)]);
    }

    const auto styleButton = [](juce::TextButton& button, juce::Colour colour)
    {
        button.setColour(juce::TextButton::buttonColourId, colour.withAlpha(0.18f));
        button.setColour(juce::TextButton::buttonOnColourId, colour.withAlpha(0.42f));
        button.setColour(juce::TextButton::textColourOffId, colour.brighter(0.7f));
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    };
    styleButton(recordButton, memoryColours[0]);
    styleButton(clearButton, juce::Colour(0xffad496a));
    styleButton(settingsButton, juce::Colour(0xff6a7c91));
    styleButton(textureButton, juce::Colour(0xffc18a55));
    styleButton(applyAudioButton, juce::Colour(0xff5da8a1));
    styleButton(rescanAudioButton, juce::Colour(0xff7895b8));
    styleButton(keyStepRoutingButton, juce::Colour(0xff8aa6d6));
    styleButton(saxModeButton, memoryColours[4]);
    styleButton(resetPerformanceLevelButton, juce::Colour(0xff8299bd));
    styleButton(toggleDelayDryButton, juce::Colour(0xff8299bd));
    styleButton(previousScenarioButton, juce::Colour(0xff8299bd));
    styleButton(nextScenarioButton, juce::Colour(0xff8299bd));
    addAndMakeVisible(recordButton);
    addAndMakeVisible(clearButton);
    addAndMakeVisible(settingsButton);
    addAndMakeVisible(textureButton);
    addChildComponent(applyAudioButton);
    addChildComponent(rescanAudioButton);
    addChildComponent(keyStepRoutingButton);
    addChildComponent(saxModeButton);
    addAndMakeVisible(resetPerformanceLevelButton);
    addAndMakeVisible(toggleDelayDryButton);

    connectionStatusLabel.setJustificationType(juce::Justification::centred);
    connectionStatusLabel.setFont(juce::FontOptions(21.0f));
    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(paleText));
    connectionStatusLabel.setText(
        "Le modifiche restano in bozza finche' non premi APPLICA AUDIO",
        juce::dontSendNotification);
    addChildComponent(connectionStatusLabel);

    hardwareRouteLabel.setJustificationType(juce::Justification::centredLeft);
    hardwareRouteLabel.setFont(juce::FontOptions(17.0f));
    hardwareRouteLabel.setMinimumHorizontalScale(0.55f);
    hardwareRouteLabel.setColour(juce::Label::textColourId, juce::Colour(paleText));
    hardwareRouteLabel.setText(
        "AUDIO NON ANCORA APERTO",
        juce::dontSendNotification);
    addChildComponent(hardwareRouteLabel);

    midiConnectionLabel.setJustificationType(juce::Justification::centred);
    midiConnectionLabel.setFont(juce::FontOptions(20.0f));
    midiConnectionLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addChildComponent(midiConnectionLabel);

    const auto makeChoice = [this](std::unique_ptr<ConnectionChoice>& target,
                                   const juce::String& name)
    {
        target = std::make_unique<ConnectionChoice>(name);
        addChildComponent(*target);
    };
    makeChoice(profileChoice, "PROFILO");
    makeChoice(backendChoice, "SISTEMA AUDIO");
    makeChoice(inputDeviceChoice, "DISPOSITIVO INGRESSO / CAPTURE");
    makeChoice(outputDeviceChoice, "DISPOSITIVO USCITA");
    makeChoice(sampleRateChoice, "FREQUENZA");
    makeChoice(bufferChoice, "BUFFER");
    makeChoice(saxInputChoice, "CANALE SAX IN");
    makeChoice(ambientOutputChoice, "USCITA AMBIENTE");
    makeChoice(bassOutputChoice, "USCITA BASSO");
    makeChoice(saxOutputChoice, "USCITA SAX");
    makeChoice(saxPathChoice, "PERCORSO SAX");
    makeChoice(diagnosticToneChoice, "TEST 997 Hz");

    recordButton.onClick = [this]
    {
        if (EcosystemEngine::isLiveBassLayer(selectedMemory))
            engine.setBassEnabled(! engine.isBassEnabled());
        else
            engine.toggleRecording(selectedMemory);
        updateControls();
    };
    clearButton.onStateChange = [this]
    {
        if (clearButton.isDown() && clearHoldStartedAt < 0.0)
        {
            clearHoldStartedAt = juce::Time::getMillisecondCounterHiRes();
            clearHoldTriggered = false;
        }
        else if (! clearButton.isDown())
        {
            clearHoldStartedAt = -1.0;
            clearHoldTriggered = false;
        }
    };
    settingsButton.onClick = [this] { toggleSettings(); };
    textureButton.onClick = [this] { cycleTexture(); };
    previousScenarioButton.onClick = [this] { changeScenario(-1); };
    nextScenarioButton.onClick = [this] { changeScenario(1); };
    applyAudioButton.onClick = [this] { applyAudioConfiguration(); };
    rescanAudioButton.onClick = [this] { scanAudioDevices(); };
    keyStepRoutingButton.onClick = [this] { configureKeyStepMidi(); };
    saxModeButton.onClick = [this]
    {
        engine.setSaxStereoInput(! engine.isSaxStereoInput());
        updateControls();
    };

    profileChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls)
            applyAudioProfile(index);
    };
    backendChoice->onChange = [this](int index)
    {
        if (updatingConnectionControls || ! juce::isPositiveAndBelow(
                index, backendNames.size()))
            return;
        audioDraft.backend = backendNames[index];
        markConnectionDraftCustom();
        refreshDeviceChoices();
    };
    inputDeviceChoice->onChange = [this](int index)
    {
        if (updatingConnectionControls)
            return;
        const auto missingDraft = audioDraft.inputDevice.isNotEmpty()
            && ! inputDeviceNames.contains(audioDraft.inputDevice);
        if (index == 0)
            audioDraft.inputDevice.clear();
        else if (! (missingDraft && index == 1))
        {
            const auto deviceIndex = index - 1 - (missingDraft ? 1 : 0);
            audioDraft.inputDevice = juce::isPositiveAndBelow(
                    deviceIndex, inputDeviceNames.size())
                ? inputDeviceNames[deviceIndex] : juce::String();
        }
        markConnectionDraftCustom();
        refreshAudioCapabilities();
    };
    outputDeviceChoice->onChange = [this](int index)
    {
        if (updatingConnectionControls)
            return;
        const auto missingDraft = audioDraft.outputDevice.isNotEmpty()
            && ! outputDeviceNames.contains(audioDraft.outputDevice);
        if (index == 0)
            audioDraft.outputDevice.clear();
        else if (! (missingDraft && index == 1))
        {
            const auto deviceIndex = index - 1 - (missingDraft ? 1 : 0);
            audioDraft.outputDevice = juce::isPositiveAndBelow(
                    deviceIndex, outputDeviceNames.size())
                ? outputDeviceNames[deviceIndex] : juce::String();
        }
        markConnectionDraftCustom();
        refreshAudioCapabilities();
    };
    sampleRateChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls
            && juce::isPositiveAndBelow(index, availableSampleRates.size()))
        {
            audioDraft.sampleRate = availableSampleRates[index];
            markConnectionDraftCustom();
        }
    };
    bufferChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls
            && juce::isPositiveAndBelow(index, availableBufferSizes.size()))
        {
            audioDraft.bufferSize = availableBufferSizes[index];
            markConnectionDraftCustom();
        }
    };
    saxInputChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls
            && juce::isPositiveAndBelow(index,
                static_cast<int>(saxInputRoutes.size())))
        {
            const auto& route = saxInputRoutes[static_cast<size_t>(index)];
            audioDraft.routing.saxInputLeft = route.left;
            audioDraft.routing.saxInputRight = route.right;
            markConnectionDraftCustom();
        }
    };
    ambientOutputChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls
            && juce::isPositiveAndBelow(index,
                static_cast<int>(outputPairRoutes.size())))
        {
            const auto& route = outputPairRoutes[static_cast<size_t>(index)];
            audioDraft.routing.ambientOutputLeft = route.left;
            audioDraft.routing.ambientOutputRight = route.right;
            markConnectionDraftCustom();
        }
    };
    bassOutputChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls
            && juce::isPositiveAndBelow(index,
                static_cast<int>(bassOutputRoutes.size())))
        {
            const auto& route = bassOutputRoutes[static_cast<size_t>(index)];
            audioDraft.routing.bassOutputLeft = route.left;
            audioDraft.routing.bassOutputRight = route.right;
            markConnectionDraftCustom();
        }
    };
    saxOutputChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls
            && juce::isPositiveAndBelow(index,
                static_cast<int>(outputPairRoutes.size())))
        {
            const auto& route = outputPairRoutes[static_cast<size_t>(index)];
            audioDraft.routing.saxOutputLeft = route.left;
            audioDraft.routing.saxOutputRight = route.right;
            markConnectionDraftCustom();
        }
    };
    saxPathChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls && index >= 0)
        {
            audioDraft.saxPath = static_cast<EcosystemEngine::SaxPathMode>(
                juce::jlimit(0, 3, index));
            markConnectionDraftCustom();
        }
    };
    diagnosticToneChoice->onChange = [this](int index)
    {
        if (! updatingConnectionControls && index >= 0)
        {
            audioDraft.tone = static_cast<EcosystemEngine::DiagnosticToneBus>(index - 1);
            markConnectionDraftCustom();
        }
    };

    decaySlider.setSliderStyle(juce::Slider::LinearHorizontal);
    decaySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 34);
    decaySlider.setRange(0.80, 1.0, 0.001);
    decaySlider.setValue(engine.getAudioDecay(), juce::dontSendNotification);
    decaySlider.setColour(juce::Slider::trackColourId, memoryColours[4]);
    decaySlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    decaySlider.onValueChange = [this]
    {
        engine.setAudioDecay(static_cast<float>(decaySlider.getValue()));
    };
    decayLabel.setText("PERSISTENZA DEL RESPIRO", juce::dontSendNotification);
    decayLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addAndMakeVisible(decaySlider);
    addAndMakeVisible(decayLabel);

    performanceLevelSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    performanceLevelSlider.setTextBoxStyle(juce::Slider::TextBoxRight,
                                           true, 112, 44);
    performanceLevelSlider.setRange(minimumPerformanceLevelDb, 0.0, 0.1);
    performanceLevelSlider.setDoubleClickReturnValue(true, 0.0);
    performanceLevelSlider.textFromValueFunction = [](double db)
    {
        return db <= minimumPerformanceLevelDb + 0.05
            ? juce::String("MUTO") : juce::String(db, 1) + " dB";
    };
    performanceLevelSlider.setColour(juce::Slider::backgroundColourId,
                                     juce::Colour(0xff202c36));
    performanceLevelSlider.setColour(juce::Slider::thumbColourId,
                                     juce::Colours::white);
    performanceLevelSlider.onValueChange = [this]
    {
        const auto gain = static_cast<float>(juce::Decibels::decibelsToGain(
            performanceLevelSlider.getValue(), minimumPerformanceLevelDb));
        engine.setPerformanceLevel(selectedMemory, gain);
        if (orbs[static_cast<size_t>(selectedMemory)] != nullptr)
            orbs[static_cast<size_t>(selectedMemory)]->repaint();
    };
    performanceLevelSlider.onDragEnd = [this]
    {
        savePerformanceLevels(true);
    };
    performanceLevelLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    performanceLevelLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(paleText));
    performanceLevelLabel.setColour(juce::Label::backgroundColourId,
                                    juce::Colour(panel));
    performanceLevelLabel.setJustificationType(juce::Justification::centred);
    performanceLevelLabel.setMinimumHorizontalScale(0.70f);
    addAndMakeVisible(performanceLevelSlider);
    addAndMakeVisible(performanceLevelLabel);
    resetPerformanceLevelButton.onClick = [this]
    {
        performanceLevelSlider.setValue(0.0, juce::sendNotificationSync);
        savePerformanceLevels(true);
    };

    delayLevelSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    delayLevelSlider.setTextBoxStyle(juce::Slider::TextBoxRight,
                                     true, 112, 44);
    delayLevelSlider.setRange(0.0, 100.0, 1.0);
    delayLevelSlider.textFromValueFunction = [](double percent)
    {
        return percent <= 0.5 ? juce::String("SENZA ECO")
                              : juce::String(static_cast<int>(percent)) + "%";
    };
    delayLevelSlider.setColour(juce::Slider::backgroundColourId,
                               juce::Colour(0xff202c36));
    delayLevelSlider.setColour(juce::Slider::thumbColourId,
                               juce::Colours::white);
    delayLevelSlider.onValueChange = [this]
    {
        engine.setDelayLevel(selectedMemory,
            static_cast<float>(delayLevelSlider.getValue() * 0.01));
        if (orbs[static_cast<size_t>(selectedMemory)] != nullptr)
            orbs[static_cast<size_t>(selectedMemory)]->repaint();
        toggleDelayDryButton.setButtonText(
            delayLevelSlider.getValue() > 0.5 ? "SENZA ECO" : "RIPRISTINA");
    };
    delayLevelSlider.onDragEnd = [this]
    {
        savePerformanceLevels(true);
    };
    delayLevelLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    delayLevelLabel.setColour(juce::Label::textColourId,
                              juce::Colour(paleText));
    delayLevelLabel.setColour(juce::Label::backgroundColourId,
                              juce::Colour(panel));
    delayLevelLabel.setJustificationType(juce::Justification::centred);
    delayLevelLabel.setMinimumHorizontalScale(0.70f);
    addAndMakeVisible(delayLevelSlider);
    addAndMakeVisible(delayLevelLabel);
    toggleDelayDryButton.onClick = [this]
    {
        const auto replacement = delayLevelSlider.getValue() > 0.5
            ? 0.0
            : static_cast<double>(defaultDelayLevels[
                  static_cast<size_t>(selectedMemory)] * 100.0f);
        delayLevelSlider.setValue(replacement, juce::sendNotificationSync);
        savePerformanceLevels(true);
    };

    const auto* savedSettings = properties.getUserSettings();
    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        const auto savedDb = savedSettings != nullptr
            ? savedSettings->getDoubleValue(
                performanceLevelSettingKeys[static_cast<size_t>(index)],
                defaultPerformanceLevelDb)
            : defaultPerformanceLevelDb;
        engine.setPerformanceLevel(index,
            static_cast<float>(juce::Decibels::decibelsToGain(
                juce::jlimit(minimumPerformanceLevelDb, 0.0, savedDb),
                minimumPerformanceLevelDb)));
        const auto savedDelay = savedSettings != nullptr
            ? savedSettings->getDoubleValue(
                delayLevelSettingKeys[static_cast<size_t>(index)],
                defaultDelayLevels[static_cast<size_t>(index)])
            : defaultDelayLevels[static_cast<size_t>(index)];
        engine.setDelayLevel(index,
            static_cast<float>(juce::jlimit(0.0, 1.0, savedDelay)));
    }
    const auto savedScenario = savedSettings != nullptr
        ? savedSettings->getIntValue("scenario", 0) : 0;
    engine.setTextureAmount(static_cast<float>(savedSettings != nullptr
        ? savedSettings->getDoubleValue("texture", 0.0) : 0.0));
    updateTextureButton();
    applyScenario(savedScenario);
    selectMemory(0);
    setSize(1280, 800);
    initialiseAudio();
    juce::Timer::startTimerHz(30);
}

MainComponent::~MainComponent()
{
    juce::Timer::stopTimer();
    savePerformanceLevels(false);
    properties.saveIfNeeded();
    deviceManager.removeMidiInputDeviceCallback({}, this);
    deviceManager.removeAudioCallback(&audioRouter);
}

void MainComponent::changeScenario(int delta)
{
    applyScenario(engine.getScenarioIndex() + delta);
}

void MainComponent::applyScenario(int index)
{
    const auto wrapped = CommentoScenarios::wrapIndex(index);
    const auto& scenario = CommentoScenarios::get(wrapped);
    engine.setScenarioIndex(wrapped);
    engine.setAudioDecay(scenario.sax.loopDecay);
    decaySlider.setValue(scenario.sax.loopDecay, juce::dontSendNotification);
    if (auto* settings = properties.getUserSettings())
    {
        settings->setValue("scenario", wrapped);
        settings->saveIfNeeded();
    }
    updateScenarioLabels();
    for (auto& orb : orbs)
        if (orb != nullptr)
            orb->repaint();
}

void MainComponent::updateScenarioLabels()
{
    const auto index = engine.getScenarioIndex();
    const auto& scenario = CommentoScenarios::get(index);
    scenarioLabel.setText(
        juce::String(index + 1).paddedLeft('0', 2) + "/"
            + juce::String(CommentoScenarios::count) + "  " + scenario.name
            + "\n" + scenario.character,
        juce::dontSendNotification);
}

void MainComponent::cycleTexture()
{
    constexpr std::array<float, 4> levels { 0.0f, 0.25f, 0.55f, 1.0f };
    const auto current = engine.getTextureAmount();
    auto next = levels.front();
    for (const auto level : levels)
        if (level > current + 0.01f)
        {
            next = level;
            break;
        }

    engine.setTextureAmount(next);
    if (auto* settings = properties.getUserSettings())
    {
        settings->setValue("texture", next);
        settings->saveIfNeeded();
    }
    updateTextureButton();
}

void MainComponent::updateTextureButton()
{
    const auto amount = engine.getTextureAmount();
    const auto name = amount < 0.1f ? "PULITA"
                    : amount < 0.4f ? "LEGGERA"
                    : amount < 0.8f ? "MEDIA" : "PIENA";
    textureButton.setButtonText("GRANA: " + juce::String(name));
    textureButton.setToggleState(amount > 0.01f, juce::dontSendNotification);
}

void MainComponent::updatePerformanceLevelControl()
{
    const auto channel = selectedMemory < EcosystemEngine::midiMemoryCount
        ? engine.getMidiChannelForMemory(selectedMemory) : 0;
    const auto name = selectedMemory == EcosystemEngine::bassLayerIndex
        ? juce::String("BASSO LIVE · MIDI 5")
        : (selectedMemory < EcosystemEngine::midiMemoryCount
            ? juce::String("PARTE · MIDI ") + juce::String(channel)
            : juce::String("SAX · INGRESSO AUDIO"));
    performanceLevelLabel.setText("LIVELLO  " + name,
                                  juce::dontSendNotification);
    performanceLevelLabel.setColour(
        juce::Label::outlineColourId,
        memoryColours[static_cast<size_t>(selectedMemory)].withAlpha(0.70f));
    performanceLevelSlider.setColour(
        juce::Slider::trackColourId,
        memoryColours[static_cast<size_t>(selectedMemory)]);

    const auto gain = engine.getPerformanceLevel(selectedMemory);
    performanceLevelSlider.setValue(
        juce::Decibels::gainToDecibels(
            static_cast<double>(gain), minimumPerformanceLevelDb),
        juce::dontSendNotification);

    delayLevelLabel.setText("DELAY  " + name, juce::dontSendNotification);
    delayLevelLabel.setColour(
        juce::Label::outlineColourId,
        memoryColours[static_cast<size_t>(selectedMemory)].withAlpha(0.70f));
    delayLevelSlider.setColour(
        juce::Slider::trackColourId,
        memoryColours[static_cast<size_t>(selectedMemory)]);
    const auto delayAmount = engine.getDelayLevel(selectedMemory);
    delayLevelSlider.setValue(delayAmount * 100.0f,
                              juce::dontSendNotification);
    delayLevelSlider.setEnabled(selectedMemory != EcosystemEngine::bassLayerIndex);
    toggleDelayDryButton.setEnabled(
        selectedMemory != EcosystemEngine::bassLayerIndex);
    toggleDelayDryButton.setButtonText(
        delayAmount > 0.005f ? "SENZA ECO" : "RIPRISTINA");
}

void MainComponent::savePerformanceLevels(bool flushToDisk)
{
    auto* settings = properties.getUserSettings();
    if (settings == nullptr)
        return;

    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        const auto db = juce::Decibels::gainToDecibels(
            static_cast<double>(engine.getPerformanceLevel(index)),
            minimumPerformanceLevelDb);
        settings->setValue(
            performanceLevelSettingKeys[static_cast<size_t>(index)], db);
        settings->setValue(
            delayLevelSettingKeys[static_cast<size_t>(index)],
            engine.getDelayLevel(index));
    }
    if (flushToDisk)
        settings->saveIfNeeded();
}

void MainComponent::initialiseAudio()
{
    // Start closed: the selected profile is applied later as one transaction.
    const auto error = deviceManager.initialise(0, 0, nullptr, false);
    configureKeyStepMidi();
    deviceManager.addMidiInputDeviceCallback({}, this);
    deviceManager.addAudioCallback(&audioRouter);

    statusLabel.setText(error.isEmpty() ? "audio pronto" : error,
                        juce::dontSendNotification);
    scanAudioDevices();
}

void MainComponent::configureKeyStepMidi()
{
    const auto devices = juce::MidiInput::getAvailableDevices();
    keyStepInputName.clear();
    juce::String selectedIdentifier;

    for (const auto& device : devices)
    {
        const auto isKeyStep = device.name.containsIgnoreCase("keystep pro")
            || (device.name.containsIgnoreCase("arturia")
                && device.name.containsIgnoreCase("keystep"));
        if (isKeyStep && selectedIdentifier.isEmpty())
        {
            selectedIdentifier = device.identifier;
            keyStepInputName = device.name;
        }
    }

    for (const auto& device : devices)
        deviceManager.setMidiInputDeviceEnabled(
            device.identifier, device.identifier == selectedIdentifier);

    midiConnectionLabel.setText(
        keyStepInputName.isNotEmpty()
            ? "ATTIVA\n" + keyStepInputName
            : "NON TROVATA\nCollega la tastiera via USB e premi RIPROVA",
        juce::dontSendNotification);
}

juce::AudioIODeviceType* MainComponent::getDraftDeviceType()
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == audioDraft.backend)
            return type;
    return nullptr;
}

void MainComponent::markConnectionDraftCustom()
{
    if (updatingConnectionControls)
        return;
    audioDraft.profile = 2;
    connectionDraftDirty = true;
    updatingConnectionControls = true;
    profileChoice->setSelectedIndex(2, false);
    updatingConnectionControls = false;
    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(0xffffd08a));
    connectionStatusLabel.setText("MODIFICHE DA APPLICARE",
                                  juce::dontSendNotification);
}

void MainComponent::syncConnectionControls()
{
    updatingConnectionControls = true;
    profileChoice->setOptions({ "MODEL 12", "STEREO GENERICO", "PERSONALIZZATO" },
                              juce::jlimit(0, 2, audioDraft.profile));

    backendChoice->setOptions(backendNames,
        juce::jmax(0, backendNames.indexOf(audioDraft.backend)));

    auto inputChoices = juce::StringArray { "NESSUNO - CAPTURE OFF" };
    const auto inputMissing = audioDraft.inputDevice.isNotEmpty()
        && ! inputDeviceNames.contains(audioDraft.inputDevice);
    if (inputMissing)
        inputChoices.add("NON TROVATO - " + audioDraft.inputDevice);
    inputChoices.addArray(inputDeviceNames);
    inputDeviceChoice->setOptions(inputChoices,
        audioDraft.inputDevice.isEmpty()
            ? 0 : (inputMissing ? 1
                : inputDeviceNames.indexOf(audioDraft.inputDevice) + 1));

    auto outputChoices = juce::StringArray { "NESSUNA USCITA" };
    const auto outputMissing = audioDraft.outputDevice.isNotEmpty()
        && ! outputDeviceNames.contains(audioDraft.outputDevice);
    if (outputMissing)
        outputChoices.add("NON TROVATA - " + audioDraft.outputDevice);
    outputChoices.addArray(outputDeviceNames);
    outputDeviceChoice->setOptions(outputChoices,
        audioDraft.outputDevice.isEmpty()
            ? 0 : (outputMissing ? 1
                : outputDeviceNames.indexOf(audioDraft.outputDevice) + 1));

    juce::StringArray rateNames;
    auto selectedRate = 0;
    for (int index = 0; index < availableSampleRates.size(); ++index)
    {
        const auto rate = availableSampleRates[index];
        rateNames.add(juce::String(rate / 1000.0, 1) + " kHz");
        if (std::abs(rate - audioDraft.sampleRate) < 1.0)
            selectedRate = index;
    }
    sampleRateChoice->setOptions(rateNames, selectedRate);

    juce::StringArray bufferNames;
    auto selectedBuffer = 0;
    for (int index = 0; index < availableBufferSizes.size(); ++index)
    {
        bufferNames.add(juce::String(availableBufferSizes[index]) + " campioni");
        if (availableBufferSizes[index] == audioDraft.bufferSize)
            selectedBuffer = index;
    }
    bufferChoice->setOptions(bufferNames, selectedBuffer);

    const auto routeNames = [](const std::vector<ChannelRouteOption>& routes)
    {
        juce::StringArray names;
        for (const auto& route : routes)
            names.add(route.name);
        return names;
    };
    const auto findRoute = [](const std::vector<ChannelRouteOption>& routes,
                              int left, int right)
    {
        for (int index = 0; index < static_cast<int>(routes.size()); ++index)
            if (routes[static_cast<size_t>(index)].left == left
                && routes[static_cast<size_t>(index)].right == right)
                return index;
        return 0;
    };

    saxInputChoice->setOptions(routeNames(saxInputRoutes),
        findRoute(saxInputRoutes, audioDraft.routing.saxInputLeft,
                  audioDraft.routing.saxInputRight));
    ambientOutputChoice->setOptions(routeNames(outputPairRoutes),
        findRoute(outputPairRoutes, audioDraft.routing.ambientOutputLeft,
                  audioDraft.routing.ambientOutputRight));
    bassOutputChoice->setOptions(routeNames(bassOutputRoutes),
        findRoute(bassOutputRoutes, audioDraft.routing.bassOutputLeft,
                  audioDraft.routing.bassOutputRight));
    saxOutputChoice->setOptions(routeNames(outputPairRoutes),
        findRoute(outputPairRoutes, audioDraft.routing.saxOutputLeft,
                  audioDraft.routing.saxOutputRight));

    saxPathChoice->setOptions(
        { "MUTO - CAPTURE RESTA APERTA", "DIRETTO PROTETTO -4.7 dB", "LOOPER PULITO",
          "EFFETTI SCENA" },
        static_cast<int>(audioDraft.saxPath));
    diagnosticToneChoice->setOptions(
        { "SPENTO", "AMBIENTE", "BASSO", "SAX" },
        static_cast<int>(audioDraft.tone) + 1);
    updatingConnectionControls = false;
}

void MainComponent::saveAudioConfiguration()
{
    auto* settings = properties.getUserSettings();
    if (settings == nullptr)
        return;

    settings->setValue("audioProfile", audioDraft.profile);
    settings->setValue("audioBackend", audioDraft.backend);
    settings->setValue("audioInputDevice", audioDraft.inputDevice);
    settings->setValue("audioOutputDevice", audioDraft.outputDevice);
    settings->setValue("audioSampleRate", audioDraft.sampleRate);
    settings->setValue("audioBufferSize", audioDraft.bufferSize);
    settings->setValue("routeSaxInputLeft", audioDraft.routing.saxInputLeft);
    settings->setValue("routeSaxInputRight", audioDraft.routing.saxInputRight);
    settings->setValue("routeAmbientLeft", audioDraft.routing.ambientOutputLeft);
    settings->setValue("routeAmbientRight", audioDraft.routing.ambientOutputRight);
    settings->setValue("routeBassLeft", audioDraft.routing.bassOutputLeft);
    settings->setValue("routeBassRight", audioDraft.routing.bassOutputRight);
    settings->setValue("routeSaxOutputLeft", audioDraft.routing.saxOutputLeft);
    settings->setValue("routeSaxOutputRight", audioDraft.routing.saxOutputRight);
    settings->setValue("saxPathMode", static_cast<int>(audioDraft.saxPath));
    // A diagnostic tone is intentionally session-only: a forgotten test must
    // never start sounding automatically at the next boot.
    settings->setValue("diagnosticTone",
                       static_cast<int>(EcosystemEngine::DiagnosticToneBus::off));
    settings->saveIfNeeded();
}

void MainComponent::refreshDeviceChoices()
{
    inputDeviceNames.clear();
    outputDeviceNames.clear();
    if (auto* type = getDraftDeviceType())
    {
        type->scanForDevices();
        outputDeviceNames = type->getDeviceNames(false);
        inputDeviceNames = type->hasSeparateInputsAndOutputs()
            ? type->getDeviceNames(true) : outputDeviceNames;
    }

    refreshAudioCapabilities();
}

void MainComponent::refreshAudioCapabilities()
{
    draftInputChannelCount = 0;
    draftOutputChannelCount = 0;
    draftInputCapabilitiesKnown = audioDraft.inputDevice.isEmpty();
    draftOutputCapabilitiesKnown = audioDraft.outputDevice.isEmpty();
    availableSampleRates.clear();
    availableBufferSizes.clear();

    const auto mergeFormats = [this](juce::AudioIODevice& device)
    {
        for (const auto rate : device.getAvailableSampleRates())
            availableSampleRates.addIfNotAlreadyThere(rate);
        for (const auto size : device.getAvailableBufferSizes())
            availableBufferSizes.addIfNotAlreadyThere(size);
    };
    const auto channelCapacity = [](juce::AudioIODevice& device, bool input)
    {
        const auto names = input ? device.getInputChannelNames()
                                 : device.getOutputChannelNames();
        const auto active = input ? device.getActiveInputChannels()
                                  : device.getActiveOutputChannels();
        return juce::jmax(names.size(), active.getHighestBit() + 1);
    };

    auto* currentDevice = deviceManager.getCurrentAudioDevice();
    const auto currentSetup = deviceManager.getAudioDeviceSetup();
    const auto sameBackend = currentDevice != nullptr
        && deviceManager.getCurrentAudioDeviceType() == audioDraft.backend;
    const auto reuseCurrentOutput = sameBackend
        && currentSetup.outputDeviceName == audioDraft.outputDevice
        && audioDraft.outputDevice.isNotEmpty();
    const auto reuseCurrentInput = sameBackend
        && currentSetup.inputDeviceName == audioDraft.inputDevice
        && audioDraft.inputDevice.isNotEmpty();

    if (reuseCurrentOutput)
    {
        draftOutputChannelCount = channelCapacity(*currentDevice, false);
        draftOutputCapabilitiesKnown = true;
        mergeFormats(*currentDevice);
    }
    if (reuseCurrentInput)
    {
        draftInputChannelCount = channelCapacity(*currentDevice, true);
        draftInputCapabilitiesKnown = true;
        mergeFormats(*currentDevice);
    }

    if (auto* type = getDraftDeviceType())
    {
        if (! draftOutputCapabilitiesKnown
            && audioDraft.outputDevice.isNotEmpty())
        {
            std::unique_ptr<juce::AudioIODevice> outputProbe(
                type->createDevice(audioDraft.outputDevice, {}));
            if (outputProbe != nullptr)
            {
                const auto count = outputProbe->getOutputChannelNames().size();
                if (count > 0)
                {
                    draftOutputChannelCount = count;
                    draftOutputCapabilitiesKnown = true;
                }
                mergeFormats(*outputProbe);
            }
        }

        if (! draftInputCapabilitiesKnown
            && audioDraft.inputDevice.isNotEmpty())
        {
            std::unique_ptr<juce::AudioIODevice> inputProbe(
                type->createDevice({}, audioDraft.inputDevice));
            if (inputProbe != nullptr)
            {
                const auto count = inputProbe->getInputChannelNames().size();
                if (count > 0)
                {
                    draftInputChannelCount = count;
                    draftInputCapabilitiesKnown = true;
                }
                mergeFormats(*inputProbe);
            }
        }
    }

    // A raw ALSA hw device may be exclusive. If a non-owning probe receives
    // EBUSY, JUCE reports zero channel names; that means "unknown", not zero.
    // Use the routing as an optimistic contiguous mask and let the real open
    // below provide the authoritative result.
    if (! draftOutputCapabilitiesKnown
        && audioDraft.outputDevice.isNotEmpty())
    {
        draftOutputChannelCount = juce::jmax(
            1, highestConfiguredOutput(audioDraft.routing) + 1);
        if (looksLikeModel12(audioDraft.outputDevice))
            draftOutputChannelCount = juce::jmax(10, draftOutputChannelCount);
        juce::Logger::writeToLog(
            "Commento: capacita' output temporaneamente ignote/busy per '"
            + audioDraft.outputDevice + "'; provo "
            + juce::String(draftOutputChannelCount) + " canali contigui");
    }
    if (! draftInputCapabilitiesKnown
        && audioDraft.inputDevice.isNotEmpty())
    {
        draftInputChannelCount = juce::jmax(
            1, highestConfiguredInput(audioDraft.routing) + 1);
        if (looksLikeModel12(audioDraft.inputDevice))
            draftInputChannelCount = juce::jmax(12, draftInputChannelCount);
        juce::Logger::writeToLog(
            "Commento: capacita' input temporaneamente ignote/busy per '"
            + audioDraft.inputDevice + "'; provo "
            + juce::String(draftInputChannelCount) + " canali contigui");
    }

    if (availableSampleRates.isEmpty())
        availableSampleRates.add(48000.0);
    if (availableBufferSizes.isEmpty())
    {
        availableBufferSizes.add(256);
        availableBufferSizes.add(512);
        availableBufferSizes.add(1024);
    }
    if (! availableSampleRates.contains(audioDraft.sampleRate))
        audioDraft.sampleRate = availableSampleRates.contains(48000.0)
            ? 48000.0 : availableSampleRates[0];
    if (! availableBufferSizes.contains(audioDraft.bufferSize))
        audioDraft.bufferSize = availableBufferSizes.contains(512)
            ? 512 : availableBufferSizes[0];

    saxInputRoutes.clear();
    saxInputRoutes.push_back({ "NESSUNO - ROUTE MUTA" });
    if (draftInputChannelCount == 0
        && audioDraft.routing.saxInputLeft
            != Model12AudioRouter::RoutingConfig::none)
    {
        auto pendingName = juce::String("IN ATTESA ")
            + juce::String(audioDraft.routing.saxInputLeft + 1);
        if (audioDraft.routing.saxInputRight
            != Model12AudioRouter::RoutingConfig::none)
            pendingName += "/"
                + juce::String(audioDraft.routing.saxInputRight + 1);
        saxInputRoutes.push_back({ pendingName,
                                   audioDraft.routing.saxInputLeft,
                                   audioDraft.routing.saxInputRight });
    }
    for (int channel = 0; channel < draftInputChannelCount; ++channel)
    {
        saxInputRoutes.push_back({ juce::String(channel + 1) + " MONO",
                                   channel,
                                   Model12AudioRouter::RoutingConfig::none });
        if (channel % 2 == 0 && channel + 1 < draftInputChannelCount)
            saxInputRoutes.push_back({ juce::String(channel + 1) + "/"
                                         + juce::String(channel + 2),
                                       channel, channel + 1 });
    }

    outputPairRoutes.clear();
    outputPairRoutes.push_back({ "NESSUNA" });
    for (int channel = 0; channel < draftOutputChannelCount; ++channel)
    {
        outputPairRoutes.push_back({ juce::String(channel + 1) + " MONO",
                                     channel, channel });
        if (channel % 2 == 0 && channel + 1 < draftOutputChannelCount)
            outputPairRoutes.push_back({ juce::String(channel + 1) + "/"
                                           + juce::String(channel + 2),
                                         channel, channel + 1 });
    }

    bassOutputRoutes.clear();
    bassOutputRoutes.push_back({ "NESSUNA" });
    for (int channel = 0; channel < draftOutputChannelCount; ++channel)
    {
        bassOutputRoutes.push_back({ juce::String(channel + 1) + " MONO",
                                     channel,
                                     Model12AudioRouter::RoutingConfig::none });
        if (channel % 2 == 0 && channel + 1 < draftOutputChannelCount)
            bassOutputRoutes.push_back({ juce::String(channel + 1) + "/"
                                           + juce::String(channel + 2)
                                           + " CENTRO",
                                         channel, channel + 1 });
    }

    const auto routeExists = [](const std::vector<ChannelRouteOption>& routes,
                                int left, int right)
    {
        for (const auto& route : routes)
            if (route.left == left && route.right == right)
                return true;
        return false;
    };
    auto& routing = audioDraft.routing;
    if (audioDraft.inputDevice.isNotEmpty() && draftInputChannelCount > 0
        && ! routeExists(saxInputRoutes, routing.saxInputLeft,
                         routing.saxInputRight))
    {
        routing.saxInputLeft = Model12AudioRouter::RoutingConfig::none;
        routing.saxInputRight = Model12AudioRouter::RoutingConfig::none;
    }
    const auto normaliseOutput = [&routeExists](
        const std::vector<ChannelRouteOption>& routes, int& left, int& right)
    {
        if (! routeExists(routes, left, right))
        {
            left = Model12AudioRouter::RoutingConfig::none;
            right = Model12AudioRouter::RoutingConfig::none;
        }
    };
    if (audioDraft.outputDevice.isNotEmpty() && draftOutputChannelCount > 0)
    {
        normaliseOutput(outputPairRoutes, routing.ambientOutputLeft,
                        routing.ambientOutputRight);
        normaliseOutput(bassOutputRoutes, routing.bassOutputLeft,
                        routing.bassOutputRight);
        normaliseOutput(outputPairRoutes, routing.saxOutputLeft,
                        routing.saxOutputRight);
    }

    syncConnectionControls();
}

void MainComponent::applyAudioProfile(int profile)
{
    profile = juce::jlimit(0, 2, profile);
    if (profile == 2)
    {
        audioDraft.profile = 2;
        connectionDraftDirty = true;
        syncConnectionControls();
        return;
    }

    if (profile == 0)
    {
        const auto isModel12 = [](const juce::String& name)
        {
            return looksLikeModel12(name)
                || name.containsIgnoreCase("tascam");
        };
        auto bestScore = -1;
        juce::String bestBackend;
        juce::String bestInput;
        juce::String bestOutput;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            const auto outputs = type->getDeviceNames(false);
            const auto inputs = type->hasSeparateInputsAndOutputs()
                ? type->getDeviceNames(true) : outputs;
            for (const auto& output : outputs)
                for (const auto& input : inputs)
                {
                    if (! isModel12(output) || ! isModel12(input))
                        continue;
                    std::unique_ptr<juce::AudioIODevice> probe(
                        type->createDevice(output, input));
                    auto score = input == output ? 20 : 0;
                    score += type->getTypeName().containsIgnoreCase("alsa") ? 20 : 0;
                    score += looksLikeModel12(input) && looksLikeModel12(output)
                        ? 200 : 0;
                    score += input.containsIgnoreCase("direct hardware")
                          || output.containsIgnoreCase("direct hardware") ? 30 : 0;
                    if (probe != nullptr
                        && probe->getInputChannelNames().size() >= 12
                        && probe->getOutputChannelNames().size() >= 10)
                        score += 100;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestBackend = type->getTypeName();
                        bestInput = input;
                        bestOutput = output;
                    }
                }
        }

        if (bestScore < 0)
        {
            lastAudioError = "Preset MODEL 12: dispositivo 12-in/10-out non trovato";
            connectionStatusLabel.setColour(juce::Label::textColourId,
                                            juce::Colour(0xffff9f8e));
            connectionStatusLabel.setText(lastAudioError,
                                          juce::dontSendNotification);
            return;
        }

        audioDraft.backend = bestBackend;
        audioDraft.inputDevice = bestInput;
        audioDraft.outputDevice = bestOutput;
        refreshDeviceChoices();
        audioDraft.routing = Model12AudioRouter::getModel12DefaultRouting();
    }
    else
    {
        if (audioDraft.backend.isEmpty() && ! backendNames.isEmpty())
            audioDraft.backend = backendNames[0];
        refreshDeviceChoices();
        if (! outputDeviceNames.contains(audioDraft.outputDevice)
            && ! outputDeviceNames.isEmpty())
            audioDraft.outputDevice = outputDeviceNames[0];
        if (inputDeviceNames.contains(audioDraft.outputDevice))
            audioDraft.inputDevice = audioDraft.outputDevice;
        else if (! inputDeviceNames.contains(audioDraft.inputDevice)
                 && ! inputDeviceNames.isEmpty())
            audioDraft.inputDevice = inputDeviceNames[0];
        refreshAudioCapabilities();
        audioDraft.routing = Model12AudioRouter::getGenericStereoRouting();
        if (draftInputChannelCount < 2)
            audioDraft.routing.saxInputRight =
                Model12AudioRouter::RoutingConfig::none;
        if (draftInputChannelCount < 1)
            audioDraft.routing.saxInputLeft =
                Model12AudioRouter::RoutingConfig::none;
        if (draftOutputChannelCount < 2)
        {
            audioDraft.routing.ambientOutputRight =
                audioDraft.routing.ambientOutputLeft;
            audioDraft.routing.bassOutputRight =
                Model12AudioRouter::RoutingConfig::none;
            audioDraft.routing.saxOutputRight =
                audioDraft.routing.saxOutputLeft;
        }
        if (draftOutputChannelCount < 1)
        {
            audioDraft.routing.ambientOutputLeft =
                Model12AudioRouter::RoutingConfig::none;
            audioDraft.routing.bassOutputLeft =
                Model12AudioRouter::RoutingConfig::none;
            audioDraft.routing.saxOutputLeft =
                Model12AudioRouter::RoutingConfig::none;
        }
    }

    audioDraft.profile = profile;
    if (availableSampleRates.contains(48000.0))
        audioDraft.sampleRate = 48000.0;
    if (availableBufferSizes.contains(512))
        audioDraft.bufferSize = 512;
    audioDraft.saxPath = EcosystemEngine::SaxPathMode::sceneEffects;
    audioDraft.tone = EcosystemEngine::DiagnosticToneBus::off;
    connectionDraftDirty = true;
    lastAudioError.clear();
    syncConnectionControls();
    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(0xffffd08a));
    connectionStatusLabel.setText("PRESET PRONTO - PREMI APPLICA AUDIO",
                                  juce::dontSendNotification);
}

void MainComponent::scanAudioDevices()
{
    const auto firstScan = audioDraft.backend.isEmpty();
    backendNames.clear();
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        backendNames.add(type->getTypeName());
    }

    if (backendNames.isEmpty())
    {
        lastAudioError = "Nessun sistema audio disponibile";
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    if (firstScan)
    {
        auto* settings = properties.getUserSettings();
        if (settings != nullptr && settings->containsKey("audioBackend"))
        {
            audioDraft.profile = juce::jlimit(
                0, 2, settings->getIntValue("audioProfile", 0));
            audioDraft.backend = settings->getValue("audioBackend");
            if (! backendNames.contains(audioDraft.backend))
                audioDraft.backend = backendNames[0];
            audioDraft.inputDevice = settings->getValue("audioInputDevice");
            audioDraft.outputDevice = settings->getValue("audioOutputDevice");
            audioDraft.sampleRate = settings->getDoubleValue(
                "audioSampleRate", 48000.0);
            audioDraft.bufferSize = settings->getIntValue("audioBufferSize", 512);
            auto& route = audioDraft.routing;
            route.saxInputLeft = settings->getIntValue("routeSaxInputLeft", 6);
            route.saxInputRight = settings->getIntValue("routeSaxInputRight", 7);
            route.ambientOutputLeft = settings->getIntValue("routeAmbientLeft", 0);
            route.ambientOutputRight = settings->getIntValue("routeAmbientRight", 1);
            route.bassOutputLeft = settings->getIntValue("routeBassLeft", 4);
            route.bassOutputRight = settings->getIntValue("routeBassRight", -1);
            route.saxOutputLeft = settings->getIntValue("routeSaxOutputLeft", 6);
            route.saxOutputRight = settings->getIntValue("routeSaxOutputRight", 7);
            audioDraft.saxPath = static_cast<EcosystemEngine::SaxPathMode>(
                juce::jlimit(0, 3, settings->getIntValue("saxPathMode", 3)));
            audioDraft.tone = EcosystemEngine::DiagnosticToneBus::off;
            refreshDeviceChoices();
        }
        else
        {
            audioDraft.backend = backendNames[0];
            refreshDeviceChoices();
            applyAudioProfile(0);
            if (lastAudioError.isNotEmpty())
            {
                lastAudioError.clear();
                applyAudioProfile(1);
            }
        }
        applyAudioConfiguration();
        return;
    }

    if (! backendNames.contains(audioDraft.backend))
        audioDraft.backend = backendNames[0];
    refreshDeviceChoices();
    connectionDraftDirty = true;
    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(0xffffd08a));
    connectionStatusLabel.setText(
        "ELENCO RILETTO - se manca un device USB riavvia Commento",
        juce::dontSendNotification);
}

void MainComponent::applyAudioConfiguration()
{
    auto* type = getDraftDeviceType();
    if (type == nullptr || audioDraft.outputDevice.isEmpty())
    {
        lastAudioError = "Scegli almeno un dispositivo di uscita";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    if (! outputDeviceNames.contains(audioDraft.outputDevice))
    {
        lastAudioError = "USCITA NON TROVATA - scegline una disponibile";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }
    if (audioDraft.inputDevice.isNotEmpty()
        && ! inputDeviceNames.contains(audioDraft.inputDevice))
    {
        lastAudioError = "INGRESSO NON TROVATO - scegli un device o CAPTURE OFF";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    refreshAudioCapabilities();
    if (audioDraft.inputDevice.isNotEmpty()
        && draftInputCapabilitiesKnown && draftInputChannelCount <= 0)
    {
        lastAudioError = "Il dispositivo scelto non dispone di ingressi audio";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }
    if (draftOutputCapabilitiesKnown && draftOutputChannelCount <= 0)
    {
        lastAudioError = "Il dispositivo scelto non espone uscite audio";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    const auto oldType = deviceManager.getCurrentAudioDeviceType();
    const auto oldSetup = deviceManager.getAudioDeviceSetup();
    const auto oldRouting = audioRouter.getRoutingConfig();
    const auto oldSaxPath = engine.getSaxPathMode();
    const auto oldTone = engine.getDiagnosticToneBus();
    auto* currentDevice = deviceManager.getCurrentAudioDevice();
    const auto oldWasOpen = currentDevice != nullptr && currentDevice->isOpen();

    const auto applyRuntimeRouting = [this]
    {
        audioRouter.setRoutingConfig(audioDraft.routing);
        engine.setSaxPathMode(audioDraft.saxPath);
        engine.setDiagnosticToneBus(audioDraft.tone);
        if ((audioDraft.saxPath == EcosystemEngine::SaxPathMode::muted
             || audioDraft.saxPath == EcosystemEngine::SaxPathMode::direct)
            && engine.isRecording(EcosystemEngine::midiMemoryCount))
            engine.toggleRecording(EcosystemEngine::midiMemoryCount);
    };
    const auto finishSuccessfulApply = [this](juce::AudioIODevice* device,
                                               bool reopened)
    {
        audioReady = device != nullptr;
        if (! audioReady)
            return false;

        refreshAudioCapabilities();
        audioDraft.sampleRate = device->getCurrentSampleRate();
        audioDraft.bufferSize = device->getCurrentBufferSizeSamples();
        connectionDraftDirty = false;
        appliedXRunBaseline = juce::jmax(0, device->getXRunCount());
        lastAudioError.clear();
        saveAudioConfiguration();
        syncConnectionControls();
        const auto captureActive = device->getActiveInputChannels()
            .countNumberOfSetBits() > 0;
        connectionStatusLabel.setColour(juce::Label::textColourId,
            captureActive
                    && audioDraft.inputDevice != audioDraft.outputDevice
                ? juce::Colour(0xffffd08a) : juce::Colour(0xff91e5c4));
        const auto captureMessage = ! captureActive
            ? juce::String("APPLICATO - CAPTURE REALMENTE SPENTA")
            : (audioDraft.inputDevice != audioDraft.outputDevice
                ? juce::String("APPLICATO - ATTENZIONE: CLOCK IN/OUT POTENZIALMENTE DIVERSI")
                : juce::String("APPLICATO - CAPTURE ATTIVA"));
        connectionStatusLabel.setText(captureMessage
                + juce::String(reopened ? " - DEVICE RIAPERTO"
                                        : " - SENZA RIAVVIO"),
            juce::dontSendNotification);
        juce::Logger::writeToLog(
            "Commento audio: backend='" + audioDraft.backend + "' input='"
            + (audioDraft.inputDevice.isEmpty() ? "NONE" : audioDraft.inputDevice)
            + "' output='" + audioDraft.outputDevice + "' rate="
            + juce::String(device->getCurrentSampleRate(), 1) + " buffer="
            + juce::String(device->getCurrentBufferSizeSamples()) + " in="
            + juce::String(device->getActiveInputChannels().countNumberOfSetBits())
            + " out="
            + juce::String(device->getActiveOutputChannels().countNumberOfSetBits())
            + (reopened ? " reopened=1" : " reopened=0"));
        return true;
    };

    const auto deviceNeedsReopen = currentDevice == nullptr
        || ! engine.isAudioRunning()
        || oldType != audioDraft.backend
        || oldSetup.inputDeviceName != audioDraft.inputDevice
        || oldSetup.outputDeviceName != audioDraft.outputDevice
        || std::abs(currentDevice->getCurrentSampleRate()
                    - audioDraft.sampleRate) > 0.5
        || currentDevice->getCurrentBufferSizeSamples() != audioDraft.bufferSize
        || ! hasContiguousChannels(currentDevice->getActiveOutputChannels(),
                                   draftOutputChannelCount)
        || audioRouter.getPhysicalOutputChannelCount() < draftOutputChannelCount
        || (audioDraft.inputDevice.isNotEmpty()
            && ! hasContiguousChannels(currentDevice->getActiveInputChannels(),
                                       draftInputChannelCount))
        || (audioDraft.inputDevice.isNotEmpty()
            && audioRouter.getPhysicalInputChannelCount()
                < draftInputChannelCount);

    if (! deviceNeedsReopen)
    {
        applyRuntimeRouting();
        finishSuccessfulApply(currentDevice, false);
        return;
    }

    for (int memory = 1; memory < EcosystemEngine::memoryCount; ++memory)
    {
        if (! engine.isRecording(memory))
            continue;
        lastAudioError = "CHIUDI LE REGISTRAZIONI PRIMA DI RIAPRIRE L'AUDIO";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    deviceManager.removeAudioCallback(&audioRouter);
    const auto restorePreviousAudio = [this, &oldType, &oldSetup,
                                       &oldRouting, oldSaxPath, oldTone,
                                       oldWasOpen]()
    {
        if (oldType.isNotEmpty()
            && deviceManager.getCurrentAudioDeviceType() != oldType)
            deviceManager.setCurrentAudioDeviceType(oldType, false);

        juce::String restoreError;
        if (oldWasOpen)
            restoreError = deviceManager.setAudioDeviceSetup(oldSetup, false);
        else
            deviceManager.closeAudioDevice();

        audioRouter.setRoutingConfig(oldRouting);
        engine.setSaxPathMode(oldSaxPath);
        engine.setDiagnosticToneBus(oldTone);
        deviceManager.addAudioCallback(&audioRouter);
        auto* restored = deviceManager.getCurrentAudioDevice();
        audioReady = restored != nullptr && restored->isOpen();
        return restoreError;
    };

    if (oldType != audioDraft.backend)
        deviceManager.setCurrentAudioDeviceType(audioDraft.backend, false);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = audioDraft.inputDevice;
    setup.outputDeviceName = audioDraft.outputDevice;
    setup.sampleRate = audioDraft.sampleRate;
    setup.bufferSize = audioDraft.bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.clear();
    if (audioDraft.inputDevice.isNotEmpty() && draftInputChannelCount > 0)
        setup.inputChannels.setRange(0, draftInputChannelCount, true);
    setup.outputChannels.setRange(0, draftOutputChannelCount, true);

    const auto error = deviceManager.setAudioDeviceSetup(setup, false);
    if (error.isNotEmpty())
    {
        const auto restoreError = restorePreviousAudio();
        lastAudioError = "APERTURA FALLITA: " + error;
        if (restoreError.isNotEmpty() || (oldWasOpen && ! audioReady))
            lastAudioError += " | RIPRISTINO FALLITO: "
                + (restoreError.isNotEmpty() ? restoreError
                                             : juce::String("device non aperto"));
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    const auto requiredOutputs = juce::jmax(
        1, highestConfiguredOutput(audioDraft.routing) + 1);
    const auto requiredInputs = audioDraft.inputDevice.isNotEmpty()
        ? juce::jmax(1, highestConfiguredInput(audioDraft.routing) + 1) : 0;
    juce::String validationError;
    if (device == nullptr || ! device->isOpen())
        validationError = "il device non e' rimasto aperto";
    else if (! hasContiguousChannels(device->getActiveOutputChannels(),
                                     requiredOutputs))
        validationError = "uscite attive insufficienti per il routing scelto";
    else if (requiredInputs > 0
             && ! hasContiguousChannels(device->getActiveInputChannels(),
                                        requiredInputs))
        validationError = "ingressi attivi insufficienti per il routing scelto";

    if (validationError.isNotEmpty())
    {
        const auto restoreError = restorePreviousAudio();
        lastAudioError = "CONFIGURAZIONE RIFIUTATA: " + validationError;
        if (restoreError.isNotEmpty() || (oldWasOpen && ! audioReady))
            lastAudioError += " | RIPRISTINO FALLITO: "
                + (restoreError.isNotEmpty() ? restoreError
                                             : juce::String("device non aperto"));
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
        return;
    }

    applyRuntimeRouting();
    deviceManager.addAudioCallback(&audioRouter);

    if (! finishSuccessfulApply(device, true))
    {
        lastAudioError = "Il sistema audio non e' rimasto aperto";
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(lastAudioError,
                                      juce::dontSendNotification);
    }
}

void MainComponent::handleIncomingMidiMessage(juce::MidiInput*,
                                               const juce::MidiMessage& message)
{
    engine.enqueueMidiMessage(message);
}

void MainComponent::timerCallback()
{
    animationPhase += 0.055;
    for (auto& orb : orbs)
    {
        orb->animation = animationPhase;
        orb->repaint();
    }

    updateClearHold();
    updateControls();
    updateHardwareIndicators();
}

void MainComponent::updateClearHold()
{
    if (! clearButton.isDown() || clearHoldStartedAt < 0.0
        || clearHoldTriggered || ! clearButton.isEnabled())
        return;

    constexpr auto holdMilliseconds = 1100.0;
    const auto elapsed = juce::Time::getMillisecondCounterHiRes()
        - clearHoldStartedAt;
    if (elapsed >= holdMilliseconds)
    {
        clearHoldTriggered = true;
        engine.clearMemory(selectedMemory);
        clearButton.setButtonText("DISSOLTA - RILASCIA");
        return;
    }

    const auto remaining = juce::jmax(0.0, holdMilliseconds - elapsed);
    clearButton.setButtonText(
        "TIENI " + juce::String(remaining / 1000.0, 1) + " s");
}

void MainComponent::updateHardwareIndicators()
{
    if (audioReady && ! engine.isAudioRunning())
    {
        audioReady = false;
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(
            "DISPOSITIVO AUDIO DISCONNESSO - ricollega e riavvia Commento",
            juce::dontSendNotification);

        // JUCE's ALSA backend caches its device list for the lifetime of the
        // process. In kiosk mode, leaving lets systemd relaunch us after the
        // launcher has seen the mixer again.
        if (juce::SystemStats::getEnvironmentVariable("COMMENTO_KIOSK", {}) == "1")
        {
            juce::Logger::writeToLog(
                "Commento: dispositivo audio disconnesso; riavvio kiosk richiesto");
            juce::JUCEApplicationBase::quit();
            return;
        }
    }

    const auto audioOk = audioReady && engine.isAudioRunning();
    const auto saxInputHot = engine.getSaxInputLevel() > 0.7079f;
    const auto saxSafetyMuted = engine.isSaxSafetyMuted();
    const auto diagnosticToneActive = engine.getDiagnosticToneBus()
        != EcosystemEngine::DiagnosticToneBus::off;
    audioStatusLabel.setText(audioOk
        ? (diagnosticToneActive ? "!  TEST 997 Hz"
            : (saxSafetyMuted ? "!  PROTEZIONE SAX"
                              : (saxInputHot ? "!  LIVELLO SAX" : "OK  AUDIO")))
        : "--  AUDIO",
                             juce::dontSendNotification);
    audioStatusLabel.setColour(
        juce::Label::backgroundColourId,
        audioOk && ! diagnosticToneActive && ! saxInputHot && ! saxSafetyMuted
            ? juce::Colour(0xff17463e) : juce::Colour(0xff482b34));

    auto keyStepPresent = false;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        if (device.name == keyStepInputName)
            keyStepPresent = true;

    const auto droppedMidi = engine.getDroppedMidiMessageCount();
    midiStatusLabel.setText(
        droppedMidi > 0 ? "!  MIDI " + juce::String(droppedMidi)
                        : (keyStepPresent ? "OK  KEYSTEP" : "--  KEYSTEP"),
        juce::dontSendNotification);
    midiStatusLabel.setColour(
        juce::Label::backgroundColourId,
        keyStepPresent && droppedMidi == 0 ? juce::Colour(0xff17463e)
                                          : juce::Colour(0xff482b34));

    if (settingsVisible)
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (! audioOk || device == nullptr)
        {
            hardwareRouteLabel.setText("NESSUN DISPOSITIVO AUDIO EFFETTIVO",
                                       juce::dontSendNotification);
            return;
        }

        const auto setup = deviceManager.getAudioDeviceSetup();
        const auto routing = audioRouter.getRoutingConfig();
        const auto routeName = [](int left, int right)
        {
            if (left < 0)
                return juce::String("NESSUNA");
            auto result = juce::String(left + 1);
            if (right == left)
                result += " MONO";
            else if (right >= 0)
                result += "/" + juce::String(right + 1);
            return result;
        };
        const auto saxPathName = [](EcosystemEngine::SaxPathMode mode)
        {
            switch (mode)
            {
                case EcosystemEngine::SaxPathMode::muted: return juce::String("MUTO");
                case EcosystemEngine::SaxPathMode::direct: return juce::String("DIRETTO -4.7 dB");
                case EcosystemEngine::SaxPathMode::cleanLooper: return juce::String("LOOPER PULITO");
                case EcosystemEngine::SaxPathMode::sceneEffects: return juce::String("FX SCENA");
            }
            return juce::String("?");
        };

        const auto activeInputs = device->getActiveInputChannels().countNumberOfSetBits();
        const auto activeOutputs = device->getActiveOutputChannels().countNumberOfSetBits();
        const auto xruns = juce::jmax(0, device->getXRunCount());
        const auto xrunDelta = juce::jmax(0, xruns - appliedXRunBaseline);
        const auto inputDb = juce::Decibels::gainToDecibels(
            engine.getSaxInputLevel(), -60.0f);
        const auto saxOutputDb = juce::Decibels::gainToDecibels(
            engine.getSaxOutputLevel(), -60.0f);
        const auto captureState = activeInputs == 0 || setup.inputDeviceName.isEmpty()
            ? juce::String("CAPTURE OFF")
            : juce::String("CAPTURE ON");
        const auto effectiveOutputName = setup.outputDeviceName.isNotEmpty()
            ? setup.outputDeviceName : device->getName();
        const auto effectiveInputName = setup.inputDeviceName.isNotEmpty()
            ? setup.inputDeviceName : juce::String("OFF");
        hardwareRouteLabel.setText(
            "EFFETTIVO · " + deviceManager.getCurrentAudioDeviceType()
                + " · IN " + effectiveInputName
                + " · OUT " + effectiveOutputName + " · "
                + juce::String(device->getCurrentSampleRate() / 1000.0, 1)
                + " kHz · " + juce::String(device->getCurrentBufferSizeSamples())
                + " · " + juce::String(activeInputs) + " IN / "
                + juce::String(activeOutputs) + " OUT · "
                + juce::String(device->getCurrentBitDepth()) + " bit · XRUN "
                + juce::String(xruns) + " (+" + juce::String(xrunDelta) + ")\n"
                + captureState + " · SAX IN "
                + routeName(routing.saxInputLeft, routing.saxInputRight)
                + " " + juce::String(inputDb, 1) + " dB"
                + " · AMB OUT "
                + routeName(routing.ambientOutputLeft, routing.ambientOutputRight)
                + " · BASS OUT "
                + routeName(routing.bassOutputLeft, routing.bassOutputRight)
                + " · SAX OUT "
                + routeName(routing.saxOutputLeft, routing.saxOutputRight)
                + " " + juce::String(saxOutputDb, 1) + " dB"
                + " · " + saxPathName(engine.getSaxPathMode()),
            juce::dontSendNotification);
    }
}

void MainComponent::selectMemory(int index)
{
    selectedMemory = juce::jlimit(0, EcosystemEngine::memoryCount - 1, index);
    for (int orbIndex = 0; orbIndex < EcosystemEngine::memoryCount; ++orbIndex)
        orbs[static_cast<size_t>(orbIndex)]->selected = orbIndex == selectedMemory;
    decaySlider.setVisible(selectedMemory == EcosystemEngine::midiMemoryCount
                           && ! settingsVisible);
    decayLabel.setVisible(decaySlider.isVisible());
    saxModeButton.setVisible(selectedMemory == EcosystemEngine::midiMemoryCount
                             && ! settingsVisible);
    updateControls();
}

void MainComponent::updateControls()
{
    const auto isBass = EcosystemEngine::isLiveBassLayer(selectedMemory);
    const auto recording = engine.isRecording(selectedMemory);
    const auto material = engine.hasMaterial(selectedMemory);
    if (isBass)
        recordButton.setButtonText(engine.isBassEnabled()
                                       ? "MUTA BASSO LIVE"
                                       : "RIATTIVA BASSO LIVE");
    else if (recording)
        recordButton.setButtonText(material ? "FERMA NUTRIMENTO" : "CHIUDI IL CICLO");
    else if (material)
        recordButton.setButtonText(selectedMemory == EcosystemEngine::midiMemoryCount
                                       ? "NUTRI" : "RISCRIVI");
    else
        recordButton.setButtonText("SEMINA");
    recordButton.setToggleState(isBass ? engine.isBassEnabled() : recording,
                                juce::dontSendNotification);
    clearButton.setEnabled(! isBass && (recording || material));
    clearButton.setVisible(! settingsVisible && ! isBass);
    if (! clearButton.isDown())
        clearButton.setButtonText("TIENI PER DISSOLVERE");

    saxModeButton.setButtonText(engine.isSaxStereoInput()
                                   ? "STEREO DALLA COPPIA"
                                   : "MONO DAL CANALE SINISTRO");

    decaySlider.setValue(engine.getAudioDecay(), juce::dontSendNotification);
    updatePerformanceLevelControl();
    updateTextureButton();

    const auto type = isBass
        ? "BASSO LIVE / MIDI 5 -> USCITA CONFIGURATA"
        : (selectedMemory < EcosystemEngine::midiMemoryCount
            ? "LOOP MIDI " + juce::String(engine.getMidiChannelForMemory(selectedMemory))
            : "SAX / ROUTING AUDIO CONFIGURATO");
    const auto count = selectedMemory > EcosystemEngine::bassLayerIndex
        && selectedMemory < EcosystemEngine::midiMemoryCount
        ? "  -  " + juce::String(engine.getEventCount(selectedMemory)) + " eventi" : "";
    statusLabel.setText(type + count, juce::dontSendNotification);
}

void MainComponent::toggleSettings()
{
    settingsVisible = ! settingsVisible;
    if (! settingsVisible
        && (audioDraft.tone != EcosystemEngine::DiagnosticToneBus::off
            || engine.getDiagnosticToneBus()
                != EcosystemEngine::DiagnosticToneBus::off))
    {
        audioDraft.tone = EcosystemEngine::DiagnosticToneBus::off;
        engine.setDiagnosticToneBus(EcosystemEngine::DiagnosticToneBus::off);
        updatingConnectionControls = true;
        diagnosticToneChoice->setSelectedIndex(0, false);
        updatingConnectionControls = false;
    }
    applyAudioButton.setVisible(settingsVisible);
    rescanAudioButton.setVisible(settingsVisible);
    keyStepRoutingButton.setVisible(settingsVisible);
    connectionStatusLabel.setVisible(settingsVisible);
    hardwareRouteLabel.setVisible(settingsVisible);
    midiConnectionLabel.setVisible(settingsVisible);
    for (auto* choice : { profileChoice.get(), backendChoice.get(),
                         inputDeviceChoice.get(), outputDeviceChoice.get(),
                         sampleRateChoice.get(), bufferChoice.get(),
                         saxInputChoice.get(), ambientOutputChoice.get(),
                         bassOutputChoice.get(), saxOutputChoice.get(),
                         saxPathChoice.get(), diagnosticToneChoice.get() })
        choice->setVisible(settingsVisible);
    for (auto& orb : orbs)
        orb->setVisible(! settingsVisible);
    recordButton.setVisible(! settingsVisible);
    clearButton.setVisible(! settingsVisible
        && ! EcosystemEngine::isLiveBassLayer(selectedMemory));
    textureButton.setVisible(! settingsVisible);
    decaySlider.setVisible(! settingsVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
    decayLabel.setVisible(decaySlider.isVisible());
    saxModeButton.setVisible(! settingsVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
    performanceLevelSlider.setVisible(! settingsVisible);
    performanceLevelLabel.setVisible(! settingsVisible);
    resetPerformanceLevelButton.setVisible(! settingsVisible);
    delayLevelSlider.setVisible(! settingsVisible);
    delayLevelLabel.setVisible(! settingsVisible);
    toggleDelayDryButton.setVisible(! settingsVisible);
    settingsButton.setButtonText(settingsVisible ? "TORNA ALLE MEMORIE" : "CONNESSIONI");
    resized();
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(background));
    juce::ColourGradient atmosphere(juce::Colour(0xff1a3141).withAlpha(0.36f),
                                    0.0f, 0.0f,
                                    juce::Colour(background).withAlpha(0.0f),
                                    static_cast<float>(getWidth()),
                                    static_cast<float>(getHeight()), false);
    graphics.setGradientFill(atmosphere);
    graphics.fillAll();

    if (settingsVisible)
    {
        graphics.setColour(juce::Colour(panel));
        graphics.fillRoundedRectangle(getLocalBounds().toFloat().reduced(42.0f)
                                      .withTrimmedTop(72.0f).withTrimmedBottom(88.0f), 18.0f);
    }
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(38);
    auto header = bounds.removeFromTop(82);
    midiStatusLabel.setBounds(header.removeFromRight(180).reduced(5, 16));
    audioStatusLabel.setBounds(header.removeFromRight(205).reduced(5, 16));
    auto titleArea = header.removeFromLeft(190);
    titleLabel.setBounds(titleArea.removeFromTop(49));
    subtitleLabel.setBounds(titleArea);
    previousScenarioButton.setBounds(header.removeFromLeft(70).reduced(5, 10));
    scenarioLabel.setBounds(header.removeFromLeft(300).reduced(5, 7));
    nextScenarioButton.setBounds(header.removeFromLeft(70).reduced(5, 10));
    statusLabel.setBounds(header.reduced(6, 0));

    auto footer = bounds.removeFromBottom(108);
    settingsButton.setBounds(footer.removeFromLeft(230).reduced(4, 14));
    textureButton.setBounds(footer.removeFromLeft(215).reduced(4, 14));
    clearButton.setBounds(footer.removeFromRight(255).reduced(4, 14));
    recordButton.setBounds(footer.withSizeKeepingCentre(390, 78));

    if (settingsVisible)
    {
        auto connectionArea = bounds.reduced(18, 10);
        connectionStatusLabel.setBounds(connectionArea.removeFromTop(54));
        auto effectiveRow = connectionArea.removeFromTop(64);
        midiConnectionLabel.setBounds(effectiveRow.removeFromRight(280).reduced(5));
        hardwareRouteLabel.setBounds(effectiveRow.reduced(5));

        auto actionRow = connectionArea.removeFromBottom(82);
        rescanAudioButton.setBounds(actionRow.removeFromLeft(285).reduced(5, 7));
        keyStepRoutingButton.setBounds(actionRow.removeFromRight(285).reduced(5, 7));
        applyAudioButton.setBounds(actionRow.withSizeKeepingCentre(390, 70));

        connectionArea.reduce(0, 6);
        constexpr int columns = 3;
        constexpr int rows = 4;
        constexpr int gap = 12;
        std::array<ConnectionChoice*, columns * rows> choices {
            profileChoice.get(), backendChoice.get(), inputDeviceChoice.get(),
            outputDeviceChoice.get(), sampleRateChoice.get(), bufferChoice.get(),
            saxInputChoice.get(), ambientOutputChoice.get(), bassOutputChoice.get(),
            saxOutputChoice.get(), saxPathChoice.get(), diagnosticToneChoice.get()
        };
        const auto rowHeight = (connectionArea.getHeight() - gap * (rows - 1)) / rows;
        for (int row = 0; row < rows; ++row)
        {
            auto rowArea = connectionArea.removeFromTop(rowHeight);
            const auto columnWidth = (rowArea.getWidth() - gap * (columns - 1)) / columns;
            for (int column = 0; column < columns; ++column)
            {
                auto cell = rowArea.removeFromLeft(
                    column == columns - 1 ? rowArea.getWidth() : columnWidth);
                choices[static_cast<size_t>(row * columns + column)]->setBounds(cell);
                if (column < columns - 1)
                    rowArea.removeFromLeft(gap);
            }
            if (row < rows - 1)
                connectionArea.removeFromTop(gap);
        }
        return;
    }

    auto performanceArea = bounds.reduced(4, 6);
    constexpr int gap = 16;
    auto controlsArea = performanceArea.removeFromBottom(136);
    auto levelArea = controlsArea.removeFromTop(64).reduced(5, 3);
    auto delayArea = controlsArea.removeFromBottom(64).reduced(5, 3);
    performanceArea.removeFromBottom(8);
    performanceLevelLabel.setBounds(levelArea.removeFromLeft(270));
    levelArea.removeFromLeft(14);
    resetPerformanceLevelButton.setBounds(
        levelArea.removeFromRight(105).reduced(3, 2));
    performanceLevelSlider.setBounds(levelArea.reduced(2, 0));
    delayLevelLabel.setBounds(delayArea.removeFromLeft(270));
    delayArea.removeFromLeft(14);
    toggleDelayDryButton.setBounds(
        delayArea.removeFromRight(105).reduced(3, 2));
    delayLevelSlider.setBounds(delayArea.reduced(2, 0));
    const auto saxHeight = juce::jlimit(170, 270,
        static_cast<int>(static_cast<float>(performanceArea.getHeight()) * 0.28f));
    auto saxArea = performanceArea.removeFromBottom(saxHeight);
    performanceArea.removeFromBottom(gap);

    const auto cardWidth = (performanceArea.getWidth() - gap * 3) / 4;
    for (int index = 0; index < EcosystemEngine::midiMemoryCount; ++index)
    {
        auto card = performanceArea.removeFromLeft(
            index == EcosystemEngine::midiMemoryCount - 1
                ? performanceArea.getWidth() : cardWidth);
        orbs[static_cast<size_t>(index)]->setBounds(card);
        if (index < EcosystemEngine::midiMemoryCount - 1)
            performanceArea.removeFromLeft(gap);
    }
    orbs[static_cast<size_t>(EcosystemEngine::midiMemoryCount)]->setBounds(saxArea);

    auto saxControls = saxArea.removeFromRight(
        juce::jmax(350, static_cast<int>(static_cast<float>(saxArea.getWidth()) * 0.34f)))
                           .reduced(26, 22);
    saxModeButton.setBounds(saxControls.removeFromTop(62));
    saxControls.removeFromTop(20);
    decayLabel.setBounds(saxControls.removeFromTop(32));
    decaySlider.setBounds(saxControls.removeFromTop(54));
}
