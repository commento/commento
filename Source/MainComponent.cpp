#include "MainComponent.h"

#include <cmath>

namespace
{
constexpr auto background = 0xff090b12;
constexpr auto panel = 0xff121725;
constexpr auto paleText = 0xffdce6e8;
constexpr auto quietText = 0xff7f9298;

const std::array<juce::Colour, EcosystemEngine::memoryCount> memoryColours {
    juce::Colour(0xff69d2e7), juce::Colour(0xffa7dbd8),
    juce::Colour(0xffe0e4cc), juce::Colour(0xfff38630),
    juce::Colour(0xffff4e50)
};
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
                ? "ATTIVO / MIDI 5 -> CH AUDIO 5"
                : "MUTO / TOCCA ATTIVA BASSO";
        else if (recording && material)
            detail = "NUTRE  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (recording)
            detail = "REGISTRA  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (material)
            detail = "SUONA  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else
            detail = index < EcosystemEngine::midiMemoryCount
                ? "VUOTA  /  MIDI " + juce::String(engine.getMidiChannelForMemory(index))
                : "VUOTA  /  AUDIO 7/8";

        graphics.setColour(recording ? juce::Colours::white : juce::Colour(quietText));
        graphics.setFont(juce::FontOptions(isSax ? 24.0f : (isBass ? 16.0f : 19.0f),
                                           juce::Font::plain));
        graphics.drawText(detail, textArea.removeFromTop(isSax ? 44.0f : 34.0f),
                          isSax ? juce::Justification::centredLeft
                                : juce::Justification::centred, false);

        const auto& scenario = CommentoScenarios::get(engine.getScenarioIndex());
        const auto timbre = isSax
            ? juce::String("SAX: ") + scenario.sax.name
            : juce::String(scenario.layers[static_cast<size_t>(index)].name);
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
                                  ? "STEREO / ingressi 7 e 8"
                                  : "MONO / ingresso 7 al centro",
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
    styleButton(model12RoutingButton, juce::Colour(0xff5da8a1));
    styleButton(keyStepRoutingButton, juce::Colour(0xff8aa6d6));
    styleButton(saxModeButton, memoryColours[4]);
    styleButton(previousScenarioButton, juce::Colour(0xff8299bd));
    styleButton(nextScenarioButton, juce::Colour(0xff8299bd));
    addAndMakeVisible(recordButton);
    addAndMakeVisible(clearButton);
    addAndMakeVisible(settingsButton);
    addChildComponent(model12RoutingButton);
    addChildComponent(keyStepRoutingButton);
    addChildComponent(saxModeButton);

    connectionStatusLabel.setJustificationType(juce::Justification::centred);
    connectionStatusLabel.setFont(juce::FontOptions(21.0f));
    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(paleText));
    connectionStatusLabel.setText(
        "Premi il pulsante per applicare il routing completo in una sola volta",
        juce::dontSendNotification);
    addChildComponent(connectionStatusLabel);

    hardwareRouteLabel.setJustificationType(juce::Justification::centred);
    hardwareRouteLabel.setFont(juce::FontOptions(22.0f));
    hardwareRouteLabel.setColour(juce::Label::textColourId, juce::Colour(paleText));
    hardwareRouteLabel.setText(
        "MODEL 12 / AUDIO\nAmbiente -> canali 1/2\nBasso -> canale 5\nSax in + out -> canali 7/8",
        juce::dontSendNotification);
    addChildComponent(hardwareRouteLabel);

    midiMapLabel.setJustificationType(juce::Justification::centred);
    midiMapLabel.setFont(juce::FontOptions(22.0f));
    midiMapLabel.setColour(juce::Label::textColourId, juce::Colour(paleText));
    midiMapLabel.setText(
        "KEYSTEP PRO\nBASSO LIVE - MIDI 5\nLOOP AMBIENT - MIDI 2 / 3 / 4",
        juce::dontSendNotification);
    addChildComponent(midiMapLabel);

    midiConnectionLabel.setJustificationType(juce::Justification::centred);
    midiConnectionLabel.setFont(juce::FontOptions(20.0f));
    midiConnectionLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addChildComponent(midiConnectionLabel);

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
    previousScenarioButton.onClick = [this] { changeScenario(-1); };
    nextScenarioButton.onClick = [this] { changeScenario(1); };
    model12RoutingButton.onClick = [this] { configureModel12Routing(); };
    keyStepRoutingButton.onClick = [this] { configureKeyStepMidi(); };
    saxModeButton.onClick = [this]
    {
        engine.setSaxStereoInput(! engine.isSaxStereoInput());
        updateControls();
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

    const auto savedScenario = properties.getUserSettings() != nullptr
        ? properties.getUserSettings()->getIntValue("scenario", 0) : 0;
    applyScenario(savedScenario);
    selectMemory(0);
    setSize(1280, 800);
    initialiseAudio();
    juce::Timer::startTimerHz(30);
}

MainComponent::~MainComponent()
{
    juce::Timer::stopTimer();
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

void MainComponent::initialiseAudio()
{
    // Do not open a default audio device: the Model 12 profile is applied as a
    // single 12-in/10-out full-duplex transaction below.
    const auto error = deviceManager.initialise(0, 0, nullptr, false);
    configureKeyStepMidi();
    deviceManager.addMidiInputDeviceCallback({}, this);
    deviceManager.addAudioCallback(&audioRouter);

    statusLabel.setText(error.isEmpty() ? "audio pronto" : error,
                        juce::dontSendNotification);
    configureModel12Routing();
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

void MainComponent::configureModel12Routing()
{
    const auto isModel12Name = [](const juce::String& name)
    {
        return name.containsIgnoreCase("model 12")
            || name.containsIgnoreCase("model12")
            || name.containsIgnoreCase("tascam");
    };

    juce::AudioIODeviceType* model12Type = nullptr;
    juce::String inputDeviceName;
    juce::String outputDeviceName;
    auto bestScore = -1;

    if (auto* current = deviceManager.getCurrentAudioDevice())
    {
        const auto currentSetup = deviceManager.getAudioDeviceSetup();
        if (isModel12Name(currentSetup.inputDeviceName)
            && isModel12Name(currentSetup.outputDeviceName)
            && current->getInputChannelNames().size() >= 12
            && current->getOutputChannelNames().size() >= 10)
        {
            model12Type = deviceManager.getCurrentDeviceTypeObject();
            inputDeviceName = currentSetup.inputDeviceName;
            outputDeviceName = currentSetup.outputDeviceName;
            bestScore = 1000;
        }
    }

    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (bestScore >= 1000)
            break;

        type->scanForDevices();
        const auto outputNames = type->getDeviceNames(false);
        const auto inputNames = type->hasSeparateInputsAndOutputs()
            ? type->getDeviceNames(true) : outputNames;
        juce::Logger::writeToLog("Commento: backend " + type->getTypeName()
            + " input=[" + inputNames.joinIntoString(" | ") + "] output=["
            + outputNames.joinIntoString(" | ") + "]");

        for (const auto& outputName : outputNames)
        {
            if (! isModel12Name(outputName))
                continue;

            for (const auto& inputName : inputNames)
            {
                if (! isModel12Name(inputName))
                    continue;

                std::unique_ptr<juce::AudioIODevice> probe(
                    type->createDevice(outputName, inputName));
                if (probe == nullptr
                    || probe->getInputChannelNames().size() < 12
                    || probe->getOutputChannelNames().size() < 10
                    || ! probe->getAvailableSampleRates().contains(48000.0))
                    continue;

                auto score = 100;
                if (type->getTypeName().containsIgnoreCase("alsa"))
                    score += 30;
                if (inputName == outputName)
                    score += 20;
                if (inputName.containsIgnoreCase("model 12")
                    || inputName.containsIgnoreCase("model12"))
                    score += 20;
                if (inputName.containsIgnoreCase("direct hardware")
                    || outputName.containsIgnoreCase("direct hardware"))
                    score += 10;

                if (score > bestScore)
                {
                    bestScore = score;
                    model12Type = type;
                    inputDeviceName = inputName;
                    outputDeviceName = outputName;
                }
            }
        }
    }

    if (model12Type == nullptr)
    {
        juce::Logger::writeToLog("Commento: nessuna Model 12 12-in/10-out trovata");
        model12Ready = false;
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(
            "MODEL 12 non trovata: controlla USB e alimentazione del mixer",
            juce::dontSendNotification);
        return;
    }

    if (deviceManager.getCurrentAudioDeviceType() != model12Type->getTypeName())
        deviceManager.setCurrentAudioDeviceType(model12Type->getTypeName(), false);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputDeviceName;
    setup.outputDeviceName = outputDeviceName;
    setup.sampleRate = 48000.0;
    setup.bufferSize = 512;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange(0, 12, true);
    setup.outputChannels.clear();
    setup.outputChannels.setRange(0, 10, true);

    const auto error = deviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
    {
        juce::Logger::writeToLog("Commento: apertura Model 12 fallita: " + error);
        model12Ready = false;
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText("Errore ALSA: " + error,
                                      juce::dontSendNotification);
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr || device->getInputChannelNames().size() < 12
        || device->getOutputChannelNames().size() < 10)
    {
        model12Ready = false;
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(
            "Dispositivo aperto, ma non espone 12 ingressi e 10 uscite",
            juce::dontSendNotification);
        deviceManager.closeAudioDevice();
        return;
    }

    model12Ready = true;
    const auto activeInputs = device->getActiveInputChannels();
    const auto activeOutputs = device->getActiveOutputChannels();
    juce::Logger::writeToLog(
        "Commento: Model 12 attiva; input='" + inputDeviceName
        + "' output='" + outputDeviceName + "' rate="
        + juce::String(device->getCurrentSampleRate(), 1) + " buffer="
        + juce::String(device->getCurrentBufferSizeSamples()) + " inputBits="
        + activeInputs.toString(2) + " outputBits=" + activeOutputs.toString(2));

    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(0xff91e5c4));
    connectionStatusLabel.setText(
        "MODEL 12 pronta: 48 kHz / 512  |  AUDIO 1/2 + 5 + 7/8",
        juce::dontSendNotification);
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
    if (model12Ready && ! engine.isAudioRunning())
    {
        model12Ready = false;
        connectionStatusLabel.setText(
            "MODEL 12 disconnessa: ricollega il mixer e riavvia Commento",
            juce::dontSendNotification);

        // JUCE's ALSA backend caches its device list for the lifetime of the
        // process. In kiosk mode, leaving lets systemd relaunch us after the
        // launcher has seen the mixer again.
        if (juce::SystemStats::getEnvironmentVariable("COMMENTO_KIOSK", {}) == "1")
        {
            juce::Logger::writeToLog(
                "Commento: Model 12 disconnessa; riavvio del kiosk richiesto");
            juce::JUCEApplicationBase::quit();
            return;
        }
    }

    const auto audioOk = model12Ready && engine.isAudioRunning();
    audioStatusLabel.setText(audioOk ? "OK  MODEL 12" : "--  MODEL 12",
                             juce::dontSendNotification);
    audioStatusLabel.setColour(
        juce::Label::backgroundColourId,
        audioOk ? juce::Colour(0xff17463e) : juce::Colour(0xff482b34));

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

    if (audioOk && settingsVisible)
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        const auto xruns = device != nullptr ? device->getXRunCount() : -1;
        const auto inputDb = juce::Decibels::gainToDecibels(
            engine.getSaxInputLevel(), -60.0f);
        const auto ambientDb = juce::Decibels::gainToDecibels(
            engine.getStereoOutputLevel(), -60.0f);
        const auto bassDb = juce::Decibels::gainToDecibels(
            engine.getBassOutputLevel(), -60.0f);
        const auto saxDb = juce::Decibels::gainToDecibels(
            engine.getSaxOutputLevel(), -60.0f);
        connectionStatusLabel.setText(
            "ATTIVA / " + juce::String(device != nullptr
                                           ? device->getCurrentSampleRate() / 1000.0 : 0.0, 1)
                + " kHz / "
                + juce::String(device != nullptr
                                   ? device->getCurrentBufferSizeSamples() : 0)
                + " campioni / xrun " + juce::String(juce::jmax(0, xruns)),
            juce::dontSendNotification);
        hardwareRouteLabel.setText(
            "MODEL 12\nCallback fisico: "
                + juce::String(audioRouter.getPhysicalInputChannelCount()) + " in / "
                + juce::String(audioRouter.getPhysicalOutputChannelCount()) + " out\n"
                + "Ambiente -> AUDIO 1/2   " + juce::String(ambientDb, 1) + " dB\n"
                + "Basso -> AUDIO 5   " + juce::String(bassDb, 1) + " dB\n"
                + "Sax IN 7/8 " + juce::String(inputDb, 1)
                + " / OUT 7/8 " + juce::String(saxDb, 1) + " dB",
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
                                       ? "SPEGNI BASSO" : "ATTIVA BASSO");
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
    if (! clearButton.isDown())
        clearButton.setButtonText(isBass ? "NESSUN LOOP SUL BASSO"
                                         : "TIENI PER DISSOLVERE");

    saxModeButton.setButtonText(engine.isSaxStereoInput()
                                   ? "STEREO 7/8" : "MONO DA INGRESSO 7");

    decaySlider.setValue(engine.getAudioDecay(), juce::dontSendNotification);

    const auto type = isBass
        ? "BASSO LIVE / MIDI 5 -> AUDIO CH 5"
        : (selectedMemory < EcosystemEngine::midiMemoryCount
            ? "LOOP MIDI " + juce::String(engine.getMidiChannelForMemory(selectedMemory))
            : "SAX / IN+OUT AUDIO 7+8");
    const auto count = selectedMemory > EcosystemEngine::bassLayerIndex
        && selectedMemory < EcosystemEngine::midiMemoryCount
        ? "  -  " + juce::String(engine.getEventCount(selectedMemory)) + " eventi" : "";
    statusLabel.setText(type + count, juce::dontSendNotification);
}

void MainComponent::toggleSettings()
{
    settingsVisible = ! settingsVisible;
    model12RoutingButton.setVisible(settingsVisible);
    keyStepRoutingButton.setVisible(settingsVisible);
    connectionStatusLabel.setVisible(settingsVisible);
    hardwareRouteLabel.setVisible(settingsVisible);
    midiMapLabel.setVisible(settingsVisible);
    midiConnectionLabel.setVisible(settingsVisible);
    for (auto& orb : orbs)
        orb->setVisible(! settingsVisible);
    recordButton.setVisible(! settingsVisible);
    clearButton.setVisible(! settingsVisible);
    decaySlider.setVisible(! settingsVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
    decayLabel.setVisible(decaySlider.isVisible());
    saxModeButton.setVisible(! settingsVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
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
        graphics.setColour(juce::Colour(0xff30404c));
        graphics.drawLine(static_cast<float>(getWidth()) * 0.55f, 150.0f,
                          static_cast<float>(getWidth()) * 0.55f,
                          static_cast<float>(getHeight() - 150), 2.0f);
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
    clearButton.setBounds(footer.removeFromRight(255).reduced(4, 14));
    recordButton.setBounds(footer.withSizeKeepingCentre(390, 78));

    if (settingsVisible)
    {
        auto connectionArea = bounds.reduced(30, 18);
        auto modelArea = connectionArea.removeFromLeft(
            static_cast<int>(static_cast<float>(connectionArea.getWidth()) * 0.54f));
        connectionArea.removeFromLeft(28);

        connectionStatusLabel.setBounds(modelArea.removeFromTop(66));
        model12RoutingButton.setBounds(modelArea.removeFromBottom(94).reduced(18, 8));
        hardwareRouteLabel.setBounds(modelArea.reduced(24, 8));

        midiMapLabel.setBounds(connectionArea.removeFromTop(118));
        keyStepRoutingButton.setBounds(
            connectionArea.removeFromBottom(94).reduced(18, 8));
        midiConnectionLabel.setBounds(connectionArea.reduced(24, 12));
        return;
    }

    auto performanceArea = bounds.reduced(4, 6);
    constexpr int gap = 16;
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
