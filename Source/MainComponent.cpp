#include "MainComponent.h"

#include <cmath>

namespace
{
constexpr auto background = 0xff090b12;
constexpr auto panel = 0xff121725;
constexpr auto paleText = 0xffdce6e8;
constexpr auto quietText = 0xff7f9298;

class CommentoLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
    {
        const auto height = buttonHeight >= 118 ? 27.0f
                          : buttonHeight >= 78 ? 22.0f
                          : buttonHeight >= 58 ? 19.0f : 16.0f;
        return juce::Font(juce::FontOptions(height, juce::Font::bold));
    }

    void drawButtonBackground(juce::Graphics& graphics,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool highlighted,
                              bool down) override
    {
        auto colour = backgroundColour;
        if (down)
            colour = colour.brighter(0.20f);
        else if (highlighted)
            colour = colour.brighter(0.09f);
        if (! button.isEnabled())
            colour = colour.withMultipliedAlpha(0.34f);

        const auto bounds = button.getLocalBounds().toFloat().reduced(1.5f);
        const auto radius = juce::jlimit(10.0f, 22.0f,
                                        bounds.getHeight() * 0.22f);
        graphics.setColour(colour);
        graphics.fillRoundedRectangle(bounds, radius);
        graphics.setColour(button.findColour(
            juce::TextButton::textColourOffId).withAlpha(
                button.isEnabled() ? 0.32f : 0.12f));
        graphics.drawRoundedRectangle(bounds, radius, down ? 2.6f : 1.4f);
    }
};

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
    juce::Colour(0xff58d68d), // BASSO LIVE: verde
    juce::Colour(0xffff9f43), // MAREA: arancione
    juce::Colour(0xffffd166), // RADICE: giallo
    juce::Colour(0xffff5c5c), // SCINTILLA: rosso
    juce::Colour(0xff58a6ff)  // RESPIRO: blu
};

const std::array<juce::String, EcosystemEngine::memoryCount> memoryNames {
    "I - BASSO LIVE", "II - MAREA", "III - RADICE", "IV - SCINTILLA",
    "RESPIRO"
};

const std::array<juce::String, EcosystemEngine::memoryCount>
    gestureTargetNames {
        "I  BASSO", "II  MAREA", "III  RADICE", "IV  SCINTILLA", "RESPIRO"
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
constexpr auto saxFootswitchRoleSettingKey = "saxFootswitchRole";
constexpr auto saxFootswitchTypeSettingKey = "saxFootswitchType";
constexpr auto saxFootswitchNumberSettingKey = "saxFootswitchNumber";

enum class MidiMonitorKind : std::uint32_t
{
    none = 0u,
    controller = 1u,
    noteOn = 2u,
    noteOff = 3u,
    other = 4u
};

[[nodiscard]] std::uint32_t packMidiMonitorMessage(
    const juce::MidiMessage& message,
    EcosystemEngine::MidiInputRole role) noexcept
{
    auto kind = MidiMonitorKind::other;
    auto number = 0;
    auto value = 0;
    if (message.isController())
    {
        kind = MidiMonitorKind::controller;
        number = message.getControllerNumber();
        value = message.getControllerValue();
    }
    else if (message.isNoteOn())
    {
        kind = MidiMonitorKind::noteOn;
        number = message.getNoteNumber();
        value = juce::jlimit(0, 127, static_cast<int>(std::round(
            message.getFloatVelocity() * 127.0f)));
    }
    else if (message.isNoteOff())
    {
        kind = MidiMonitorKind::noteOff;
        number = message.getNoteNumber();
    }

    return static_cast<std::uint32_t>(kind)
        | (static_cast<std::uint32_t>(role) & 0x3u) << 3u
        | (static_cast<std::uint32_t>(juce::jlimit(
               0, 16, message.getChannel())) & 0x1fu) << 5u
        | (static_cast<std::uint32_t>(juce::jlimit(0, 127, number))
            & 0x7fu) << 10u
        | (static_cast<std::uint32_t>(juce::jlimit(0, 127, value))
            & 0x7fu) << 17u;
}

[[nodiscard]] juce::String midiRoleText(
    EcosystemEngine::MidiInputRole role)
{
    switch (role)
    {
        case EcosystemEngine::MidiInputRole::keyStep: return "KEYSTEP PRO";
        case EcosystemEngine::MidiInputRole::model12: return "MODEL 12";
        case EcosystemEngine::MidiInputRole::nm2: return "NM2";
        case EcosystemEngine::MidiInputRole::generic: return "MIDI";
    }
    return "MIDI";
}

bool isMidiControlEndpoint(const juce::String& name)
{
    return name.containsIgnoreCase("daw")
        || name.containsIgnoreCase("control")
        || name.containsIgnoreCase("ctrl");
}

bool isModel12SecondaryMidiEndpoint(const juce::String& name)
{
    // Windows exposes the DAW port as "MIDIIN2 (Model 12 MIDI)" without the
    // words DAW/CONTROL. Linux may suffix the two USB MIDI ports with 1/2.
    // The DIN MIDI IN is port 1; port 2 belongs to DAW control.
    return name.containsIgnoreCase("midiin2")
        || name.containsIgnoreCase("midi in 2")
        || name.containsIgnoreCase("midi 2")
        || name.containsIgnoreCase("port 2");
}

bool looksLikeKeyStepMidi(const juce::String& name)
{
    return ! isMidiControlEndpoint(name)
        && (name.containsIgnoreCase("keystep pro")
            || (name.containsIgnoreCase("arturia")
                && name.containsIgnoreCase("keystep")));
}

bool looksLikeModel12Midi(const juce::String& name)
{
    return looksLikeModel12(name)
        && ! isMidiControlEndpoint(name)
        && ! isModel12SecondaryMidiEndpoint(name);
}

bool looksLikeNm2Midi(const juce::String& name)
{
    return name.containsIgnoreCase("nm2")
        || name.containsIgnoreCase("this is noise")
        || name.containsIgnoreCase("this.is.noise")
        || name.containsIgnoreCase("thisisnoise");
}

bool looksLikeBluetoothMidiEndpoint(const juce::String& description)
{
    return description.containsIgnoreCase("bluetooth")
        || description.containsIgnoreCase("ble midi")
        || description.startsWithIgnoreCase("ble ")
        || description.endsWithIgnoreCase(" ble")
        || description.containsIgnoreCase("(ble)");
}

EcosystemEngine::MidiInputRole midiInputRoleForName(const juce::String& name)
{
    if (looksLikeNm2Midi(name))
        return EcosystemEngine::MidiInputRole::nm2;
    if (looksLikeKeyStepMidi(name))
        return EcosystemEngine::MidiInputRole::keyStep;
    if (looksLikeModel12Midi(name))
        return EcosystemEngine::MidiInputRole::model12;
    return EcosystemEngine::MidiInputRole::generic;
}

bool sameSaxFootswitchBinding(
    const EcosystemEngine::SaxFootswitchBinding& first,
    const EcosystemEngine::SaxFootswitchBinding& second) noexcept
{
    return first.role == second.role
        && first.type == second.type
        && first.number == second.number;
}

juce::String saxFootswitchBindingText(
    const EcosystemEngine::SaxFootswitchBinding& binding)
{
    if (! binding.valid())
        return "PEDALE SAX: NON ASSOCIATO";

    juce::String source;
    switch (binding.role)
    {
        case EcosystemEngine::MidiInputRole::keyStep: source = "KEYSTEP PRO"; break;
        case EcosystemEngine::MidiInputRole::model12: source = "MODEL 12"; break;
        case EcosystemEngine::MidiInputRole::nm2: source = "NM2"; break;
        case EcosystemEngine::MidiInputRole::generic: source = "MIDI"; break;
    }

    juce::String message;
    switch (binding.type)
    {
        case EcosystemEngine::SaxFootswitchMessageType::controller:
            message = "CC " + juce::String(binding.number);
            break;
        case EcosystemEngine::SaxFootswitchMessageType::none:
            break;
    }
    return "PEDALE SAX: " + source + " / " + message;
}

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
        const auto isBass = EcosystemEngine::isLiveBassLayer(index);
        const auto phase = engine.getPhase(index);
        const auto recording = engine.isRecording(index);
        const auto waitingForFirstNote = juce::isPositiveAndBelow(
                index, EcosystemEngine::midiMemoryCount)
            && engine.isWaitingForFirstNote(index);
        const auto captureActive = recording || waitingForFirstNote;
        const auto material = engine.hasMaterial(index);
        const auto bassActive = isBass && engine.isBassEnabled();
        const auto breathing = 1.0f + 0.035f * std::sin(static_cast<float>(animation * 2.0
                                                    + static_cast<double>(index)));

        juce::ColourGradient surface(
            colour.withAlpha(captureActive ? 0.26f
                : (material || bassActive ? 0.14f : 0.06f)),
            bounds.getX(), bounds.getY(), juce::Colour(panel).darker(0.32f),
            bounds.getRight(), bounds.getBottom(), false);
        graphics.setGradientFill(surface);
        graphics.fillRoundedRectangle(bounds, 24.0f);
        graphics.setColour(captureActive ? juce::Colours::white.withAlpha(0.92f)
            : colour.withAlpha(selected || bassActive ? 0.95f : 0.34f));
        graphics.drawRoundedRectangle(bounds.reduced(selected ? 3.0f : 2.0f), 22.0f,
                                      selected ? 6.0f : 2.5f);

        const auto isSax = index == EcosystemEngine::midiMemoryCount;
        auto phaseArea = isSax
            ? bounds.removeFromLeft(juce::jmin(260.0f, bounds.getHeight()))
            : bounds.withTrimmedTop(54.0f).withTrimmedBottom(86.0f);
        const auto centre = phaseArea.getCentre();
        const auto radius = 0.38f * juce::jmin(phaseArea.getWidth(), phaseArea.getHeight());

        juce::ColourGradient glow(colour.withAlpha(captureActive ? 0.52f : 0.24f),
                                  centre.x, centre.y, colour.withAlpha(0.0f),
                                  centre.x + radius * 1.35f, centre.y, true);
        graphics.setGradientFill(glow);
        graphics.fillEllipse(juce::Rectangle<float>(radius * 2.0f * breathing,
                                                     radius * 2.0f * breathing)
                                 .withCentre(centre));
        graphics.setColour(colour.withAlpha(
            material || captureActive || bassActive ? 0.70f : 0.20f));
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
                ? "ATTIVO  /  MIDI 5  /  LIVE"
                : "MUTO  /  MIDI 5";
        else if (waitingForFirstNote)
            detail = "ATTENDO NOTA  /  MIDI "
                + juce::String(engine.getMidiChannelForMemory(index));
        else if (recording && material)
            detail = "NUTRI / OVERDUB  /  "
                + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (recording)
            detail = "CATTURA  /  "
                + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (material && ! engine.isLoopPlaying(index))
            detail = "IN PAUSA  /  "
                + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (material)
            detail = "SUONA  /  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else
            detail = index < EcosystemEngine::midiMemoryCount
                ? "VUOTA  /  MIDI " + juce::String(engine.getMidiChannelForMemory(index))
                : "VUOTA  /  INGRESSO CONFIGURATO";

        if (material && ! captureActive)
        {
            const auto evolution = engine.getLoopEvolution(index);
            if (evolution == EcosystemEngine::LoopEvolution::octaveUp)
                detail += "  /  OMBRA +12";
            else if (evolution == EcosystemEngine::LoopEvolution::reverse)
                detail += "  /  OMBRA REVERSE";

            if (engine.getThinnedMemoryIndex() == index)
                detail += "  /  RESPIRA";
        }

        graphics.setColour(captureActive ? juce::Colours::white
                                         : juce::Colour(quietText));
        graphics.setFont(juce::FontOptions(isSax ? 24.0f
                                           : (isBass ? 16.0f : 19.0f),
                                           juce::Font::plain));
        graphics.drawText(detail, textArea.removeFromTop(isSax ? 44.0f : 34.0f),
                          isSax ? juce::Justification::centredLeft
                                : juce::Justification::centred, false);

        const auto& scenario = CommentoScenarios::get(engine.getScenarioIndex());
        auto timbre = isSax
            ? juce::String("SAX: ") + scenario.sax.name
            : juce::String(scenario.layers[static_cast<size_t>(index)].name);
        timbre += "  |  " + performanceLevelText(
            engine.getPerformanceLevel(index));
        if (! isBass)
            timbre += "  |  " + delayLevelText(engine.getDelayLevel(index));
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
    interfaceLookAndFeel = std::make_unique<CommentoLookAndFeel>();
    setLookAndFeel(interfaceLookAndFeel.get());
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

    subtitleLabel.setText("tre memorie, un canale live e un respiro",
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

    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        orbs[static_cast<size_t>(index)] = std::make_unique<MemoryOrb>(
            index, memoryNames[static_cast<size_t>(index)],
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
    styleButton(gesturesButton, juce::Colour(0xff9b7ed9));
    const auto persistentGestureColour = juce::Colour(0xffa996e8);
    const auto momentaryGestureColour = juce::Colour(0xff526173);
    styleButton(textureButton, persistentGestureColour);
    styleButton(fuzzButton, persistentGestureColour);
    styleButton(evolutionButton, persistentGestureColour);
    styleButton(freezeButton, momentaryGestureColour);
    styleButton(echoThrowButton, momentaryGestureColour);
    styleButton(freeTailButton, momentaryGestureColour);
    styleButton(thinningButton, persistentGestureColour);
    styleButton(saxListenButton, persistentGestureColour);
    styleButton(loopTransportButton, juce::Colour(0xff5da8a1));
    styleButton(applyAudioButton, juce::Colour(0xff5da8a1));
    styleButton(rescanAudioButton, juce::Colour(0xff7895b8));
    styleButton(keyStepRoutingButton, juce::Colour(0xff8aa6d6));
    styleButton(saxFootswitchLearnButton, memoryColours[4]);
    styleButton(saxFootswitchClearButton, juce::Colour(0xffad496a));
    styleButton(saxModeButton, memoryColours[4]);
    styleButton(resetPerformanceLevelButton, juce::Colour(0xff8299bd));
    styleButton(toggleDelayDryButton, juce::Colour(0xff8299bd));
    styleButton(previousScenarioButton, juce::Colour(0xff8299bd));
    styleButton(nextScenarioButton, juce::Colour(0xff8299bd));
    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        auto button = std::make_unique<juce::TextButton>(
            gestureTargetNames[static_cast<std::size_t>(index)]);
        styleButton(*button, memoryColours[static_cast<std::size_t>(index)]);
        button->onClick = [this, index] { selectMemory(index); };
        addChildComponent(*button);
        gestureTargetButtons[static_cast<std::size_t>(index)]
            = std::move(button);
    }
    addAndMakeVisible(recordButton);
    addAndMakeVisible(clearButton);
    addAndMakeVisible(settingsButton);
    addAndMakeVisible(gesturesButton);
    addAndMakeVisible(textureButton);
    addAndMakeVisible(fuzzButton);
    addAndMakeVisible(evolutionButton);
    addAndMakeVisible(freezeButton);
    addAndMakeVisible(echoThrowButton);
    addAndMakeVisible(freeTailButton);
    addAndMakeVisible(thinningButton);
    addAndMakeVisible(saxListenButton);
    addAndMakeVisible(loopTransportButton);
    addChildComponent(applyAudioButton);
    addChildComponent(rescanAudioButton);
    addChildComponent(keyStepRoutingButton);
    addChildComponent(saxFootswitchLearnButton);
    addChildComponent(saxFootswitchClearButton);
    addChildComponent(saxModeButton);
    addAndMakeVisible(resetPerformanceLevelButton);
    addAndMakeVisible(toggleDelayDryButton);

    gesturesTitleLabel.setText("GESTI", juce::dontSendNotification);
    gesturesTitleLabel.setFont(juce::FontOptions(38.0f, juce::Font::bold));
    gesturesTitleLabel.setColour(juce::Label::textColourId,
                                 juce::Colour(paleText));
    gesturesTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addChildComponent(gesturesTitleLabel);

    gesturesHintLabel.setText(
        "TRASFORMAZIONI E TRASPORTO  /  TIENI I PAD MOMENTANEI  /  UN SOLO BERSAGLIO",
        juce::dontSendNotification);
    gesturesHintLabel.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    gesturesHintLabel.setColour(juce::Label::textColourId,
                                juce::Colour(quietText));
    gesturesHintLabel.setJustificationType(juce::Justification::centredLeft);
    gesturesHintLabel.setMinimumHorizontalScale(0.55f);
    addChildComponent(gesturesHintLabel);

    gestureTargetLabel.setFont(juce::FontOptions(21.0f, juce::Font::bold));
    gestureTargetLabel.setColour(juce::Label::textColourId,
                                 juce::Colour(paleText));
    gestureTargetLabel.setColour(juce::Label::backgroundColourId,
                                 juce::Colour(panel));
    gestureTargetLabel.setJustificationType(juce::Justification::centred);
    addChildComponent(gestureTargetLabel);

    sustainMonitorLabel.setFont(juce::FontOptions(19.0f, juce::Font::bold));
    sustainMonitorLabel.setColour(juce::Label::textColourId,
                                  juce::Colour(quietText));
    sustainMonitorLabel.setJustificationType(juce::Justification::centredLeft);
    sustainMonitorLabel.setMinimumHorizontalScale(0.65f);
    addChildComponent(sustainMonitorLabel);

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
    midiConnectionLabel.setMinimumHorizontalScale(0.55f);
    addChildComponent(midiConnectionLabel);

    saxFootswitchBindingLabel.setJustificationType(
        juce::Justification::centredLeft);
    saxFootswitchBindingLabel.setFont(juce::FontOptions(17.0f,
                                                        juce::Font::bold));
    saxFootswitchBindingLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(paleText));
    saxFootswitchBindingLabel.setColour(juce::Label::backgroundColourId,
                                        juce::Colour(0xff18202d));
    saxFootswitchBindingLabel.setMinimumHorizontalScale(0.55f);
    addChildComponent(saxFootswitchBindingLabel);

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
    makeChoice(bassOutputChoice, "USCITA CANALE I (BASSO / SAX)");
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
    gesturesButton.onClick = [this] { toggleGestures(); };
    textureButton.onClick = [this] { cycleTexture(); };
    fuzzButton.onClick = [this]
    {
        engine.setFuzzEnabled(! engine.isFuzzEnabled());
        updateControls();
    };
    evolutionButton.onClick = [this]
    {
        engine.setLoopEvolutionEnabled(! engine.isLoopEvolutionEnabled());
        updateControls();
    };
    freezeButton.onStateChange = [this]
    {
        if (freezeButton.isDown())
        {
            if (touchscreenFreezeTarget < 0)
            {
                touchscreenFreezeTarget = selectedMemory;
                engine.setFreezeEnabled(touchscreenFreezeTarget, true);
            }
        }
        else if (touchscreenFreezeTarget >= 0)
        {
            engine.setFreezeEnabled(touchscreenFreezeTarget, false);
            touchscreenFreezeTarget = -1;
        }
    };
    echoThrowButton.onStateChange = [this]
    {
        if (echoThrowButton.isDown())
        {
            if (touchscreenEchoThrowTarget < 0)
            {
                touchscreenEchoThrowTarget = selectedMemory;
                engine.setEchoThrowEnabled(touchscreenEchoThrowTarget, true);
            }
        }
        else if (touchscreenEchoThrowTarget >= 0)
        {
            engine.setEchoThrowEnabled(touchscreenEchoThrowTarget, false);
            touchscreenEchoThrowTarget = -1;
        }
    };
    freeTailButton.onStateChange = [this]
    {
        if (freeTailButton.isDown())
        {
            const auto saxEffectsAvailable =
                selectedMemory != EcosystemEngine::midiMemoryCount
                || engine.getSaxPathMode()
                    == EcosystemEngine::SaxPathMode::sceneEffects;
            if (touchscreenFreeTailTarget < 0
                && ! EcosystemEngine::isLiveBassLayer(selectedMemory)
                && saxEffectsAvailable)
            {
                touchscreenFreeTailTarget = selectedMemory;
                engine.setFreeTailEnabled(touchscreenFreeTailTarget, true);
            }
        }
        else if (touchscreenFreeTailTarget >= 0)
        {
            engine.setFreeTailEnabled(touchscreenFreeTailTarget, false);
            touchscreenFreeTailTarget = -1;
        }
    };
    thinningButton.onClick = [this]
    {
        engine.setThinningEnabled(! engine.isThinningEnabled());
        updateControls();
    };
    saxListenButton.onClick = [this] { toggleSaxListening(); };
    loopTransportButton.onClick = [this]
    {
        const auto isBass = EcosystemEngine::isLiveBassLayer(selectedMemory);
        const auto waitingForFirstNote = juce::isPositiveAndBelow(
                selectedMemory, EcosystemEngine::midiMemoryCount)
            && engine.isWaitingForFirstNote(selectedMemory);
        if (! isBass && engine.hasMaterial(selectedMemory)
            && ! engine.isRecording(selectedMemory) && ! waitingForFirstNote)
        {
            engine.setLoopPlaying(selectedMemory,
                                  ! engine.isLoopPlaying(selectedMemory));
            updateControls();
        }
    };
    previousScenarioButton.onClick = [this] { changeScenario(-1); };
    nextScenarioButton.onClick = [this] { changeScenario(1); };
    applyAudioButton.onClick = [this] { applyAudioConfiguration(); };
    rescanAudioButton.onClick = [this] { scanAudioDevices(); };
    keyStepRoutingButton.onClick = [this] { configureKeyStepMidi(); };
    saxFootswitchLearnButton.onClick = [this]
    {
        if (engine.isSaxFootswitchLearning())
            engine.cancelSaxFootswitchLearn();
        else
            engine.beginSaxFootswitchLearn();
        updateSaxFootswitchControls();
    };
    saxFootswitchClearButton.onClick = [this]
    {
        engine.cancelSaxFootswitchLearn();
        engine.releaseSaxFootswitch();
        engine.clearSaxFootswitchBinding();
        saveSaxFootswitchBinding(true);
        updateSaxFootswitchControls();
    };
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
    decayLabel.setText("NUTRI: AGGIUNGE E CONSUMA LENTAMENTE LA MEMORIA",
                       juce::dontSendNotification);
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
        const auto defaultAmount = defaultDelayLevels[
            static_cast<size_t>(selectedMemory)];
        const auto replacement = delayLevelSlider.getValue() > 0.5
            ? 0.0
            : static_cast<double>(defaultAmount * 100.0f);
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
    loadSaxFootswitchBinding();
    updateSaxFootswitchControls();
    updateTextureButton();
    applyScenario(savedScenario);
    selectMemory(0);
    updatePageVisibility();
    updateMidiMonitor();
    setSize(1600, 1000);
    initialiseAudio();
    juce::Timer::startTimerHz(30);
}

MainComponent::~MainComponent()
{
    juce::Timer::stopTimer();
    deviceManager.removeMidiInputDeviceCallback({}, this);
    engine.releaseNm2Gestures();
    savePerformanceLevels(false);
    saveSaxFootswitchBinding(false);
    properties.saveIfNeeded();
    deviceManager.removeAudioCallback(&audioRouter);
    setLookAndFeel(nullptr);
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
    // The engine now carries PERSISTENZA to the destination together with the
    // rest of the scene. Updating only the visual target here avoids the old
    // abrupt jump at the instant an arrow is touched.
    decaySlider.setValue(scenario.sax.loopDecay, juce::dontSendNotification);
    if (auto* settings = properties.getUserSettings())
    {
        settings->setValue("scenario", wrapped);
        settings->saveIfNeeded();
    }
    updateScenarioLabels();
    updateControls();
    for (auto& orb : orbs)
        if (orb != nullptr)
            orb->repaint();
}

void MainComponent::updateScenarioLabels()
{
    const auto requestedIndex = engine.getScenarioIndex();
    const auto index = engine.getScenarioMorphDestinationIndex();
    const auto& scenario = CommentoScenarios::get(index);
    const auto progress = engine.getScenarioMorphProgress();
    const auto sourceIndex = engine.getScenarioMorphSourceIndex();
    juce::String text;
    if (progress < 0.999f && sourceIndex != index)
    {
        const auto& source = CommentoScenarios::get(sourceIndex);
        text = juce::String(source.name) + "  ->  " + scenario.name
             + "  " + juce::String(static_cast<int>(std::round(
                    progress * 100.0f))) + "%"
             + (requestedIndex != index
                    ? juce::String("  |  POI ")
                        + CommentoScenarios::get(requestedIndex).name
                    : juce::String())
             + "\n"
             + scenario.character;
    }
    else
        text = juce::String(index + 1).paddedLeft('0', 2) + "/"
            + juce::String(CommentoScenarios::count) + "  " + scenario.name
            + "\n" + scenario.character;
    if (scenarioLabel.getText() != text)
        scenarioLabel.setText(text, juce::dontSendNotification);
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

void MainComponent::toggleSaxListening()
{
    engine.setSaxListenAmount(engine.getSaxListenAmount() > 0.01f
        ? 0.0f : 1.0f);
    updateControls();
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
            ? juce::String("BASSO LIVE | MIDI 5")
        : (selectedMemory < EcosystemEngine::midiMemoryCount
            ? juce::String("PARTE | MIDI ") + juce::String(channel)
            : juce::String("SAX | INGRESSO AUDIO"));
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

void MainComponent::loadSaxFootswitchBinding()
{
    auto binding = EcosystemEngine::SaxFootswitchBinding {};
    if (const auto* settings = properties.getUserSettings())
    {
        binding.role = static_cast<EcosystemEngine::MidiInputRole>(
            settings->getIntValue(saxFootswitchRoleSettingKey,
                static_cast<int>(EcosystemEngine::MidiInputRole::generic)));
        binding.type = static_cast<EcosystemEngine::SaxFootswitchMessageType>(
            settings->getIntValue(saxFootswitchTypeSettingKey,
                static_cast<int>(EcosystemEngine::SaxFootswitchMessageType::none)));
        binding.number = settings->getIntValue(
            saxFootswitchNumberSettingKey, -1);
    }

    if (binding.valid())
        engine.setSaxFootswitchBinding(binding);
    else
        engine.clearSaxFootswitchBinding();
    persistedSaxFootswitchBinding = engine.getSaxFootswitchBinding();
}

void MainComponent::saveSaxFootswitchBinding(bool flushToDisk)
{
    auto* settings = properties.getUserSettings();
    if (settings == nullptr)
        return;

    const auto binding = engine.getSaxFootswitchBinding();
    settings->setValue(saxFootswitchRoleSettingKey,
                       static_cast<int>(binding.role));
    settings->setValue(saxFootswitchTypeSettingKey,
                       static_cast<int>(binding.type));
    settings->setValue(saxFootswitchNumberSettingKey, binding.number);
    if (flushToDisk)
        settings->saveIfNeeded();
    persistedSaxFootswitchBinding = binding;
}

void MainComponent::updateSaxFootswitchControls()
{
    const auto learning = engine.isSaxFootswitchLearning();
    const auto binding = engine.getSaxFootswitchBinding();
    saxFootswitchLearnButton.setButtonText(
        learning ? "ANNULLA APPRENDIMENTO" : "IMPARA PEDALE SAX");
    saxFootswitchLearnButton.setToggleState(learning,
                                            juce::dontSendNotification);
    saxFootswitchClearButton.setEnabled(binding.valid());
    saxFootswitchBindingLabel.setText(
        learning ? "PEDALE SAX: APPRENDIMENTO - PREMI IL PEDALE"
                 : saxFootswitchBindingText(binding),
        juce::dontSendNotification);
    saxFootswitchBindingLabel.setColour(
        juce::Label::textColourId,
        learning ? juce::Colour(0xffffd08a) : juce::Colour(paleText));
}

void MainComponent::updateMidiMonitor()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto sustainPacked = lastSustainValue.load(std::memory_order_relaxed);
    if (sustainPacked >= 0)
    {
        const auto value = sustainPacked & 0x7f;
        const auto role = static_cast<EcosystemEngine::MidiInputRole>(
            (sustainPacked >> 8) & 0x3);
        const auto edges = sustainEdgeMask.load(std::memory_order_relaxed);
        const auto completeCycle = (edges & 0x3u) == 0x3u;
        sustainMonitorLabel.setText(
            "PEDALE RICEVUTO  /  " + midiRoleText(role)
                + "  /  CC64 = " + juce::String(value)
                + (value >= 64 ? "  PREMUTO" : "  RILASCIATO")
                + (completeCycle ? "  /  CICLO 127-0 OK"
                                 : "  /  MUOVILO FINO AL RILASCIO"),
            juce::dontSendNotification);
        const auto recent = static_cast<std::uint32_t>(
            now - lastSustainTick.load(std::memory_order_relaxed)) < 900u;
        sustainMonitorLabel.setColour(
            juce::Label::textColourId,
            recent ? memoryColours[0].brighter(0.35f)
                   : juce::Colour(paleText));
        return;
    }

    const auto packed = lastMidiMessagePacked.load(std::memory_order_relaxed);
    const auto kind = static_cast<MidiMonitorKind>(packed & 0x7u);
    if (kind == MidiMonitorKind::none)
    {
        sustainMonitorLabel.setText(
            "PEDALE  /  NESSUN MIDI RICEVUTO  /  PREMI UNA NOTA E POI IL SUSTAIN",
            juce::dontSendNotification);
        sustainMonitorLabel.setColour(juce::Label::textColourId,
                                      juce::Colour(quietText));
        return;
    }

    const auto role = static_cast<EcosystemEngine::MidiInputRole>(
        (packed >> 3u) & 0x3u);
    const auto channel = static_cast<int>((packed >> 5u) & 0x1fu);
    const auto number = static_cast<int>((packed >> 10u) & 0x7fu);
    const auto value = static_cast<int>((packed >> 17u) & 0x7fu);
    auto event = juce::String("MIDI ") + midiRoleText(role) + " OK  /  ";
    if (kind == MidiMonitorKind::controller)
        event += "CC" + juce::String(number) + " = " + juce::String(value);
    else if (kind == MidiMonitorKind::noteOn
             || kind == MidiMonitorKind::noteOff)
        event += "NOTA " + juce::String(number)
            + (kind == MidiMonitorKind::noteOn ? " ON" : " OFF")
            + "  CH " + juce::String(channel);
    else
        event += "MESSAGGIO MIDI";
    sustainMonitorLabel.setText(
        event + "  /  CC64 NON ANCORA RICEVUTO",
        juce::dontSendNotification);
    const auto recent = static_cast<std::uint32_t>(
        now - lastMidiMessageTick.load(std::memory_order_relaxed)) < 900u;
    sustainMonitorLabel.setColour(juce::Label::textColourId,
        recent ? juce::Colour(0xffffd08a) : juce::Colour(quietText));
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
    // Re-reading or losing a controller is also the explicit panic path for a
    // missing release. The saved footswitch assignment is deliberately kept.
    engine.releaseMomentaryGestures();
    engine.releaseNm2Gestures();
    engine.releaseSaxFootswitch();
    engine.cancelSaxFootswitchLearn();
    lastMidiMessagePacked.store(0u, std::memory_order_relaxed);
    lastMidiMessageTick.store(0u, std::memory_order_relaxed);
    lastSustainValue.store(-1, std::memory_order_relaxed);
    lastSustainTick.store(0u, std::memory_order_relaxed);
    sustainEdgeMask.store(0u, std::memory_order_relaxed);
    const auto devices = juce::MidiInput::getAvailableDevices();
    keyStepInputName.clear();
    model12MidiInputName.clear();
    nm2MidiInputName.clear();
    nm2MidiInputIdentifier.clear();
    nm2InputWasPresent = false;
    juce::String keyStepIdentifier;
    juce::String model12Identifier;
    auto nm2IsBluetooth = false;

    for (const auto& device : devices)
    {
        if (looksLikeKeyStepMidi(device.name) && keyStepIdentifier.isEmpty())
        {
            keyStepIdentifier = device.identifier;
            keyStepInputName = device.name;
        }
        else if (looksLikeModel12Midi(device.name)
                 && model12Identifier.isEmpty())
        {
            model12Identifier = device.identifier;
            model12MidiInputName = device.name;
        }

        if (looksLikeNm2Midi(device.name))
        {
            const auto candidateIsBluetooth = looksLikeBluetoothMidiEndpoint(
                device.name + " " + device.identifier);
            if (nm2MidiInputIdentifier.isEmpty()
                || (nm2IsBluetooth && ! candidateIsBluetooth))
            {
                nm2MidiInputIdentifier = device.identifier;
                nm2MidiInputName = device.name;
                nm2IsBluetooth = candidateIsBluetooth;
            }
        }
    }

    nm2InputWasPresent = nm2MidiInputIdentifier.isNotEmpty();

    for (const auto& device : devices)
    {
        const auto shouldEnable = device.identifier == keyStepIdentifier
            || device.identifier == model12Identifier
            || device.identifier == nm2MidiInputIdentifier;
        deviceManager.setMidiInputDeviceEnabled(
            device.identifier, shouldEnable);
    }

    juce::StringArray activeSources;
    if (keyStepInputName.isNotEmpty())
        activeSources.add("KEYSTEP PRO");
    if (model12MidiInputName.isNotEmpty())
        activeSources.add("MODEL 12");
    if (nm2MidiInputName.isNotEmpty())
        activeSources.add(nm2IsBluetooth ? "NM2 BLE" : "NM2");

    const auto connectionText = activeSources.isEmpty()
        ? juce::String("MIDI NON TROVATO\nCollega KeyStep Pro, Model 12 o NM2")
        : juce::String(activeSources.size())
            + (activeSources.size() == 1
                ? " PORTA MIDI ATTIVA\n" : " PORTE MIDI ATTIVE\n")
            + activeSources.joinIntoString(" + ");
    midiConnectionLabel.setText(connectionText, juce::dontSendNotification);
    updateSaxFootswitchControls();
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

void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source,
                                               const juce::MidiMessage& message)
{
    const auto role = source != nullptr
        ? midiInputRoleForName(source->getName())
        : EcosystemEngine::MidiInputRole::generic;
    if (! message.isActiveSense() && ! message.isMidiClock())
    {
        lastMidiMessagePacked.store(
            packMidiMonitorMessage(message, role), std::memory_order_relaxed);
        lastMidiMessageTick.store(
            juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
        if (message.isController()
            && message.getControllerNumber() == 64)
        {
            lastSustainValue.store(
                message.getControllerValue()
                    | (static_cast<int>(role) << 8),
                                   std::memory_order_relaxed);
            sustainEdgeMask.fetch_or(
                message.getControllerValue() >= 64 ? 2u : 1u,
                std::memory_order_relaxed);
            lastSustainTick.store(
                juce::Time::getMillisecondCounter(),
                std::memory_order_relaxed);
        }
    }
    engine.enqueueMidiMessage(message, role);
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
    updateScenarioLabels();
    const auto binding = engine.getSaxFootswitchBinding();
    if (! sameSaxFootswitchBinding(binding, persistedSaxFootswitchBinding))
        saveSaxFootswitchBinding(true);
    updateSaxFootswitchControls();
    updateMidiMonitor();
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
    auto model12MidiPresent = false;
    auto nm2EndpointPresent = false;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
    {
        if (device.name == keyStepInputName)
            keyStepPresent = true;
        if (device.name == model12MidiInputName)
            model12MidiPresent = true;
        if (device.identifier == nm2MidiInputIdentifier)
            nm2EndpointPresent = true;
    }

    if (nm2InputWasPresent && ! nm2EndpointPresent)
    {
        // A missing note-off over BLE/USB must not leave a held gesture behind.
        engine.releaseNm2Gestures();
        nm2InputWasPresent = false;
        midiConnectionLabel.setText(
            "NM2 DISCONNESSO\nRicollega e premi RILEGGI MIDI",
            juce::dontSendNotification);
    }

    const auto droppedMidi = engine.getDroppedMidiMessageCount();
    const auto midiInputCount = static_cast<int>(keyStepPresent)
        + static_cast<int>(model12MidiPresent)
        + static_cast<int>(nm2InputWasPresent && nm2EndpointPresent);
    midiStatusLabel.setText(
        droppedMidi > 0 ? "!  MIDI " + juce::String(droppedMidi)
                        : (midiInputCount > 0
                            ? "OK  MIDI " + juce::String(midiInputCount)
                            : "--  MIDI"),
        juce::dontSendNotification);
    midiStatusLabel.setColour(
        juce::Label::backgroundColourId,
        midiInputCount > 0 && droppedMidi == 0 ? juce::Colour(0xff17463e)
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
        const auto realtimeState = engine.getRealtimeSchedulingStatus();
        const auto realtimeText = realtimeState > 0
            ? juce::String("PRIORITA AUDIO OK")
            : (realtimeState == 0 ? juce::String("PRIORITA AUDIO NON ATTIVA")
                                  : juce::String("PRIORITA AUDIO IN ATTESA"));
        const auto dspPercent = juce::jlimit(
            0, 999, static_cast<int>(std::round(engine.getDspLoad() * 100.0f)));
        const auto dspNearOverloads = engine.getDspNearOverloadCount();
        const auto callbackIntervalPercent = juce::jlimit(
            0, 999, static_cast<int>(std::round(
                engine.getCallbackIntervalLoad() * 100.0f)));
        const auto lateCallbacks = engine.getLateCallbackCount();
        const auto effectiveOutputName = setup.outputDeviceName.isNotEmpty()
            ? setup.outputDeviceName : device->getName();
        const auto effectiveInputName = setup.inputDeviceName.isNotEmpty()
            ? setup.inputDeviceName : juce::String("OFF");
        hardwareRouteLabel.setText(
            "EFFETTIVO | " + deviceManager.getCurrentAudioDeviceType()
                + " | IN " + effectiveInputName
                + " | OUT " + effectiveOutputName + " | "
                + juce::String(device->getCurrentSampleRate() / 1000.0, 1)
                + " kHz | " + juce::String(device->getCurrentBufferSizeSamples())
                + " | " + juce::String(activeInputs) + " IN / "
                + juce::String(activeOutputs) + " OUT | "
                + juce::String(device->getCurrentBitDepth()) + " bit | XRUN "
                + juce::String(xruns) + " (+" + juce::String(xrunDelta) + ")"
                + " | DSP " + juce::String(dspPercent) + "%"
                + " | PICCHI " + juce::String(dspNearOverloads)
                + " | INTERVALLO " + juce::String(callbackIntervalPercent) + "%"
                + " | RITARDI " + juce::String(lateCallbacks)
                + " | " + realtimeText + "\n"
                + captureState + " | SAX IN "
                + routeName(routing.saxInputLeft, routing.saxInputRight)
                + " " + juce::String(inputDb, 1) + " dB"
                + " | AMB OUT "
                + routeName(routing.ambientOutputLeft, routing.ambientOutputRight)
                + " | CH I OUT "
                + routeName(routing.bassOutputLeft, routing.bassOutputRight)
                + " | SAX OUT "
                + routeName(routing.saxOutputLeft, routing.saxOutputRight)
                + " " + juce::String(saxOutputDb, 1) + " dB"
                + " | " + saxPathName(engine.getSaxPathMode()),
            juce::dontSendNotification);
    }
}

void MainComponent::selectMemory(int index)
{
    selectedMemory = juce::jlimit(0, EcosystemEngine::memoryCount - 1, index);
    engine.setGestureTarget(selectedMemory);
    for (int orbIndex = 0; orbIndex < EcosystemEngine::memoryCount; ++orbIndex)
        orbs[static_cast<size_t>(orbIndex)]->selected = orbIndex == selectedMemory;
    updatePageVisibility();
    updateControls();
}

void MainComponent::updateControls()
{
    const auto selectedColour = memoryColours[
        static_cast<size_t>(selectedMemory)];
    recordButton.setColour(juce::TextButton::buttonColourId,
                           selectedColour.withAlpha(0.18f));
    recordButton.setColour(juce::TextButton::buttonOnColourId,
                           selectedColour.withAlpha(0.42f));
    recordButton.setColour(juce::TextButton::textColourOffId,
                           selectedColour.brighter(0.7f));

    const auto isBass = EcosystemEngine::isLiveBassLayer(selectedMemory);
    const auto isMidiLoopMemory = juce::isPositiveAndBelow(
        selectedMemory, EcosystemEngine::midiMemoryCount);
    const auto recording = engine.isRecording(selectedMemory);
    const auto waitingForFirstNote = isMidiLoopMemory
        && engine.isWaitingForFirstNote(selectedMemory);
    const auto material = engine.hasMaterial(selectedMemory);
    recordButton.setTriggeredOnMouseDown(isMidiLoopMemory);
    if (isBass)
        recordButton.setButtonText(engine.isBassEnabled()
                                       ? "MUTA BASSO LIVE"
                                       : "RIATTIVA BASSO LIVE");
    else if (waitingForFirstNote)
        recordButton.setButtonText("ATTENDO NOTA");
    else if (recording)
        recordButton.setButtonText(material ? "FERMA NUTRI" : "CHIUDI IL CICLO");
    else if (material)
        recordButton.setButtonText(selectedMemory == EcosystemEngine::midiMemoryCount
                                       ? "NUTRI / OVERDUB" : "RISCRIVI");
    else
        recordButton.setButtonText("SEMINA");
    recordButton.setToggleState(isBass ? engine.isBassEnabled()
                                       : (recording || waitingForFirstNote),
                                juce::dontSendNotification);
    clearButton.setEnabled(! isBass && (recording || material));
    clearButton.setVisible(! settingsVisible && ! gesturesVisible && ! isBass);
    if (! clearButton.isDown())
        clearButton.setButtonText("TIENI PER DISSOLVERE");

    saxModeButton.setButtonText(engine.isSaxStereoInput()
                                   ? "STEREO DALLA COPPIA"
                                   : "MONO DAL CANALE SINISTRO");

    if (! decaySlider.isMouseButtonDown())
        decaySlider.setValue(engine.getAudioDecay(), juce::dontSendNotification);
    updatePerformanceLevelControl();
    updateTextureButton();
    fuzzButton.setButtonText(engine.isFuzzEnabled()
                                 ? "FUZZ: ATTIVO" : "FUZZ: SPENTO");
    fuzzButton.setToggleState(engine.isFuzzEnabled(),
                              juce::dontSendNotification);
    evolutionButton.setButtonText(engine.isLoopEvolutionEnabled()
                                      ? "DERIVA: RARA" : "DERIVA: SPENTA");
    evolutionButton.setToggleState(engine.isLoopEvolutionEnabled(),
                                   juce::dontSendNotification);
    const auto saxEffectsAvailable = selectedMemory != EcosystemEngine::midiMemoryCount
        || engine.getSaxPathMode() == EcosystemEngine::SaxPathMode::sceneEffects;
    const auto gesturesAvailable = ! isBass && saxEffectsAvailable;
    freezeButton.setEnabled(gesturesAvailable);
    echoThrowButton.setEnabled(gesturesAvailable);
    freeTailButton.setEnabled(gesturesAvailable);
    const auto freezeTarget = touchscreenFreezeTarget >= 0
        ? touchscreenFreezeTarget : selectedMemory;
    const auto echoTarget = touchscreenEchoThrowTarget >= 0
        ? touchscreenEchoThrowTarget : selectedMemory;
    const auto freezeActive = engine.isFreezeEnabled(freezeTarget);
    const auto echoActive = engine.isEchoThrowEnabled(echoTarget);
    freezeButton.setButtonText(freezeActive
        ? "GELO: ATTIVO" : "GELO: TIENI");
    echoThrowButton.setButtonText(echoActive
        ? "ECO: LANCIO" : "ECO THROW");
    freezeButton.setToggleState(freezeActive,
                                juce::dontSendNotification);
    echoThrowButton.setToggleState(echoActive,
                                    juce::dontSendNotification);
    const auto freeTailTarget = touchscreenFreeTailTarget >= 0
        ? touchscreenFreeTailTarget : selectedMemory;
    const auto freeTailActive = engine.isFreeTailEnabled(freeTailTarget);
    freeTailButton.setButtonText(freeTailActive
        ? "CODA LIBERA: ATTIVA" : "CODA LIBERA: TIENI");
    freeTailButton.setToggleState(freeTailActive,
                                  juce::dontSendNotification);
    const auto thinningEnabled = engine.isThinningEnabled();
    const auto thinnedMemory = engine.getThinnedMemoryIndex();
    thinningButton.setButtonText(! thinningEnabled
        ? (thinnedMemory >= 1 ? "DIRADA: FINISCE" : "DIRADA: SPENTA")
        : (thinnedMemory >= 1 ? "DIRADA: RESPIRA" : "DIRADA: ATTIVA"));
    thinningButton.setToggleState(thinningEnabled,
                                  juce::dontSendNotification);
    const auto listening = engine.getSaxListenAmount();
    saxListenButton.setButtonText(listening <= 0.005f
        ? "ASCOLTO: SPENTO"
        : "ASCOLTO: " + juce::String(static_cast<int>(
              std::round(listening * 100.0f))) + "%");
    saxListenButton.setToggleState(listening > 0.005f,
                                   juce::dontSendNotification);
    const auto loopPlaying = isBass || engine.isLoopPlaying(selectedMemory);
    loopTransportButton.setEnabled(! isBass && material && ! recording
                                   && ! waitingForFirstNote);
    loopTransportButton.setButtonText(loopPlaying
        ? "PAUSA LOOP" : "PLAY LOOP");
    loopTransportButton.setToggleState(! loopPlaying,
                                       juce::dontSendNotification);

    const auto nm2HeldMask = engine.getNm2HeldMask();
    if (nm2HeldMask != 0u)
    {
        juce::StringArray heldGestures;
        for (int index = 0; index < EcosystemEngine::nm2GestureCount; ++index)
            if ((nm2HeldMask & (1u << static_cast<unsigned int>(index))) != 0u)
                heldGestures.add(EcosystemEngine::getNm2GestureName(
                    static_cast<EcosystemEngine::Nm2Gesture>(index)));
        gesturesHintLabel.setText(
            "NM2 PREMUTO  /  " + heldGestures.joinIntoString(" + ")
                + "  /  RILASCIA PER USCIRE",
            juce::dontSendNotification);
        gesturesHintLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(0xffffd08a));
    }
    else
    {
        gesturesHintLabel.setText(
            nm2InputWasPresent
                ? "NM2 PRONTO  /  TIENI I PAD MOMENTANEI  /  UN SOLO BERSAGLIO"
                : "TRASFORMAZIONI E TRASPORTO  /  TIENI I PAD MOMENTANEI  /  UN SOLO BERSAGLIO",
            juce::dontSendNotification);
        gesturesHintLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(quietText));
    }

    gestureTargetLabel.setText(
        "BERSAGLIO  /  " + memoryNames[static_cast<std::size_t>(selectedMemory)]
            + (isBass
                ? "  /  I PAD MOMENTANEI NON ELABORANO IL BASSO"
                : "  /  IL PAD CATTURA QUESTA MEMORIA"),
        juce::dontSendNotification);
    gestureTargetLabel.setColour(
        juce::Label::outlineColourId, selectedColour.withAlpha(0.72f));
    for (auto* button : { &freezeButton, &echoThrowButton, &freeTailButton })
    {
        button->setColour(juce::TextButton::buttonOnColourId,
                          selectedColour.withAlpha(0.54f));
        button->setColour(juce::TextButton::textColourOnId,
                          juce::Colours::white);
    }
    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
        gestureTargetButtons[static_cast<std::size_t>(index)]->setToggleState(
            index == selectedMemory, juce::dontSendNotification);

    const auto type = waitingForFirstNote
        ? "LOOP MIDI " + juce::String(engine.getMidiChannelForMemory(selectedMemory))
            + " / ATTENDO PRIMO NOTE-ON"
        : (isBass
            ? "BASSO LIVE / MIDI 5 -> USCITA CONFIGURATA"
        : (selectedMemory < EcosystemEngine::midiMemoryCount
            ? "LOOP MIDI " + juce::String(engine.getMidiChannelForMemory(selectedMemory))
            : (material
                ? "SAX / NUTRI: AGGIUNGE E CONSUMA LENTAMENTE LA MEMORIA"
                : "SAX / ROUTING AUDIO CONFIGURATO")));
    const auto count = ! waitingForFirstNote
        && selectedMemory > EcosystemEngine::bassLayerIndex
        && selectedMemory < EcosystemEngine::midiMemoryCount
        ? "  -  " + juce::String(engine.getEventCount(selectedMemory)) + " eventi" : "";
    statusLabel.setText(type + count, juce::dontSendNotification);
}

void MainComponent::toggleSettings()
{
    settingsVisible = ! settingsVisible;
    gesturesVisible = false;
    // Never leave a hidden learn or a held edge behind when crossing pages.
    engine.releaseSaxFootswitch();
    engine.cancelSaxFootswitchLearn();
    engine.releaseMomentaryGestures();
    touchscreenFreezeTarget = -1;
    touchscreenEchoThrowTarget = -1;
    touchscreenFreeTailTarget = -1;
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
    updatePageVisibility();
    updateControls();
    resized();
    repaint();
}

void MainComponent::toggleGestures()
{
    gesturesVisible = ! gesturesVisible;
    settingsVisible = false;
    // A hidden momentary pad must never remain held. The DSP performs the
    // release ramp after these ownership bits are cleared.
    engine.releaseMomentaryGestures();
    touchscreenFreezeTarget = -1;
    touchscreenEchoThrowTarget = -1;
    touchscreenFreeTailTarget = -1;
    engine.releaseSaxFootswitch();
    engine.cancelSaxFootswitchLearn();
    updatePageVisibility();
    updateControls();
    resized();
    repaint();
}

void MainComponent::updatePageVisibility()
{
    const auto performanceVisible = ! settingsVisible && ! gesturesVisible;

    applyAudioButton.setVisible(settingsVisible);
    rescanAudioButton.setVisible(settingsVisible);
    keyStepRoutingButton.setVisible(settingsVisible);
    connectionStatusLabel.setVisible(settingsVisible);
    hardwareRouteLabel.setVisible(settingsVisible);
    midiConnectionLabel.setVisible(settingsVisible);
    saxFootswitchBindingLabel.setVisible(settingsVisible || gesturesVisible);
    saxFootswitchLearnButton.setVisible(settingsVisible || gesturesVisible);
    saxFootswitchClearButton.setVisible(settingsVisible || gesturesVisible);
    for (auto* choice : { profileChoice.get(), backendChoice.get(),
                         inputDeviceChoice.get(), outputDeviceChoice.get(),
                         sampleRateChoice.get(), bufferChoice.get(),
                         saxInputChoice.get(), ambientOutputChoice.get(),
                         bassOutputChoice.get(), saxOutputChoice.get(),
                         saxPathChoice.get(), diagnosticToneChoice.get() })
        choice->setVisible(settingsVisible);
    for (auto& orb : orbs)
        orb->setVisible(performanceVisible);
    recordButton.setVisible(performanceVisible);
    clearButton.setVisible(performanceVisible
        && ! EcosystemEngine::isLiveBassLayer(selectedMemory));
    textureButton.setVisible(gesturesVisible);
    fuzzButton.setVisible(gesturesVisible);
    evolutionButton.setVisible(gesturesVisible);
    freezeButton.setVisible(gesturesVisible);
    echoThrowButton.setVisible(gesturesVisible);
    freeTailButton.setVisible(gesturesVisible);
    thinningButton.setVisible(gesturesVisible);
    saxListenButton.setVisible(gesturesVisible);
    loopTransportButton.setVisible(gesturesVisible);
    gesturesTitleLabel.setVisible(gesturesVisible);
    gesturesHintLabel.setVisible(gesturesVisible);
    gestureTargetLabel.setVisible(gesturesVisible);
    sustainMonitorLabel.setVisible(gesturesVisible);
    for (auto& button : gestureTargetButtons)
        button->setVisible(gesturesVisible);
    decaySlider.setVisible(performanceVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
    decayLabel.setVisible(decaySlider.isVisible());
    saxModeButton.setVisible(performanceVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
    performanceLevelSlider.setVisible(performanceVisible);
    performanceLevelLabel.setVisible(performanceVisible);
    resetPerformanceLevelButton.setVisible(performanceVisible);
    delayLevelSlider.setVisible(performanceVisible);
    delayLevelLabel.setVisible(performanceVisible);
    toggleDelayDryButton.setVisible(performanceVisible);

    settingsButton.setButtonText(settingsVisible
        ? "TORNA ALLE MEMORIE" : "CONNESSIONI");
    gesturesButton.setVisible(! settingsVisible);
    gesturesButton.setButtonText(gesturesVisible
        ? "TORNA ALLE MEMORIE" : "GESTI");
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

    const auto drawPanel = [&graphics](juce::Rectangle<int> bounds,
                                       juce::Colour accent,
                                       float alpha = 0.30f)
    {
        if (bounds.isEmpty())
            return;
        const auto area = bounds.toFloat();
        graphics.setColour(juce::Colour(panel).withAlpha(0.96f));
        graphics.fillRoundedRectangle(area, 22.0f);
        graphics.setColour(accent.withAlpha(alpha));
        graphics.drawRoundedRectangle(area.reduced(1.0f), 21.0f, 1.7f);
    };

    if (settingsVisible)
    {
        graphics.setColour(juce::Colour(panel));
        graphics.fillRoundedRectangle(getLocalBounds().toFloat().reduced(42.0f)
                                      .withTrimmedTop(72.0f).withTrimmedBottom(88.0f), 18.0f);
    }
    else if (gesturesVisible)
    {
        drawPanel(gesturesMainPanelBounds, juce::Colour(0xffa996e8));
        drawPanel(gesturesPedalPanelBounds, memoryColours[4], 0.24f);
    }
    else
        drawPanel(saxControlPanelBounds, memoryColours[4], 0.34f);
}

void MainComponent::resized()
{
    saxControlPanelBounds = {};
    gesturesMainPanelBounds = {};
    gesturesPedalPanelBounds = {};

    const auto margin = getWidth() < 1500 ? 28 : 38;
    auto bounds = getLocalBounds().reduced(margin);
    auto header = bounds.removeFromTop(92);
    auto titleArea = header.removeFromLeft(240);
    titleLabel.setBounds(titleArea.removeFromTop(49));
    subtitleLabel.setBounds(titleArea);

    auto indicators = header.removeFromRight(390);
    midiStatusLabel.setBounds(indicators.removeFromRight(180).reduced(5, 13));
    indicators.removeFromRight(8);
    audioStatusLabel.setBounds(indicators.reduced(5, 13));

    const auto completeHeader = getLocalBounds().reduced(margin)
        .removeFromTop(92);
    const auto scenarioWidth = juce::jmin(640,
        juce::jmax(420, completeHeader.getWidth() - 690));
    auto scenarioArea = completeHeader.withSizeKeepingCentre(
        scenarioWidth, 82);
    previousScenarioButton.setBounds(
        scenarioArea.removeFromLeft(78).reduced(4, 5));
    nextScenarioButton.setBounds(
        scenarioArea.removeFromRight(78).reduced(4, 5));
    scenarioLabel.setBounds(scenarioArea.reduced(7, 4));
    const auto statusLeft = completeHeader.getX() + 240;
    const auto statusRight = scenarioLabel.getX() - 10;
    statusLabel.setBounds(statusLeft, completeHeader.getY(),
                          juce::jmax(0, statusRight - statusLeft),
                          completeHeader.getHeight());

    auto footer = bounds.removeFromBottom(108);
    const auto completeFooter = footer;
    settingsButton.setBounds(footer.removeFromLeft(210).reduced(4, 12));
    footer.removeFromLeft(10);
    gesturesButton.setBounds(footer.removeFromLeft(210).reduced(4, 12));
    clearButton.setBounds(footer.removeFromRight(230).reduced(4, 12));
    recordButton.setBounds(completeFooter.withSizeKeepingCentre(400, 78));

    if (settingsVisible)
    {
        auto connectionArea = bounds.reduced(18, 10);
        connectionStatusLabel.setBounds(connectionArea.removeFromTop(44));
        auto effectiveRow = connectionArea.removeFromTop(52);
        midiConnectionLabel.setBounds(effectiveRow.removeFromRight(340).reduced(5));
        hardwareRouteLabel.setBounds(effectiveRow.reduced(5));

        auto pedalRow = connectionArea.removeFromTop(54);
        saxFootswitchClearButton.setBounds(
            pedalRow.removeFromRight(150).reduced(4, 3));
        saxFootswitchLearnButton.setBounds(
            pedalRow.removeFromRight(275).reduced(4, 3));
        saxFootswitchBindingLabel.setBounds(pedalRow.reduced(8, 3));

        auto actionRow = connectionArea.removeFromBottom(70);
        rescanAudioButton.setBounds(actionRow.removeFromLeft(285).reduced(5));
        keyStepRoutingButton.setBounds(actionRow.removeFromRight(285).reduced(5));
        applyAudioButton.setBounds(actionRow.withSizeKeepingCentre(390, 60));

        connectionArea.reduce(0, 4);
        constexpr int columns = 3;
        constexpr int rows = 4;
        constexpr int gap = 8;
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

    if (gesturesVisible)
    {
        auto gestureArea = bounds.reduced(16, 10);
        const auto compactGestures = gestureArea.getHeight() < 600;
        gesturesTitleLabel.setBounds(gestureArea.removeFromTop(
            compactGestures ? 44 : 48));
        gesturesHintLabel.setBounds(gestureArea.removeFromTop(
            compactGestures ? 24 : 30));
        gestureArea.removeFromTop(compactGestures ? 6 : 10);
        gestureTargetLabel.setBounds(gestureArea.removeFromTop(
            compactGestures ? 34 : 42));
        auto targetRow = gestureArea.removeFromTop(
            compactGestures ? 48 : 64);
        constexpr int targetGap = 12;
        const auto targetWidth = (targetRow.getWidth()
            - targetGap * (EcosystemEngine::memoryCount - 1))
            / EcosystemEngine::memoryCount;
        for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
        {
            auto cell = targetRow.removeFromLeft(
                index == EcosystemEngine::memoryCount - 1
                    ? targetRow.getWidth() : targetWidth);
            gestureTargetButtons[static_cast<std::size_t>(index)]->setBounds(
                cell);
            if (index < EcosystemEngine::memoryCount - 1)
                targetRow.removeFromLeft(targetGap);
        }
        const auto sectionGap = compactGestures ? 8 : 18;
        gestureArea.removeFromTop(sectionGap);

        const auto mainTop = gestureTargetLabel.getY() - 10;
        auto globalRow = gestureArea.removeFromTop(
            compactGestures ? 62 : 112);
        constexpr int gestureGap = 18;
        const auto globalWidth = (globalRow.getWidth() - gestureGap * 2) / 3;
        textureButton.setBounds(globalRow.removeFromLeft(globalWidth));
        globalRow.removeFromLeft(gestureGap);
        fuzzButton.setBounds(globalRow.removeFromLeft(globalWidth));
        globalRow.removeFromLeft(gestureGap);
        evolutionButton.setBounds(globalRow);

        gestureArea.removeFromTop(sectionGap);
        auto momentaryRow = gestureArea.removeFromTop(
            compactGestures ? 72 : 156);
        const auto momentaryWidth
            = (momentaryRow.getWidth() - gestureGap * 2) / 3;
        freezeButton.setBounds(momentaryRow.removeFromLeft(momentaryWidth));
        momentaryRow.removeFromLeft(gestureGap);
        echoThrowButton.setBounds(momentaryRow.removeFromLeft(momentaryWidth));
        momentaryRow.removeFromLeft(gestureGap);
        freeTailButton.setBounds(momentaryRow);

        gestureArea.removeFromTop(sectionGap);
        auto automaticRow = gestureArea.removeFromTop(
            compactGestures ? 58 : 104);
        const auto automaticWidth
            = (automaticRow.getWidth() - gestureGap * 2) / 3;
        thinningButton.setBounds(automaticRow.removeFromLeft(automaticWidth));
        automaticRow.removeFromLeft(gestureGap);
        saxListenButton.setBounds(automaticRow.removeFromLeft(automaticWidth));
        automaticRow.removeFromLeft(gestureGap);
        loopTransportButton.setBounds(automaticRow);
        gesturesMainPanelBounds = juce::Rectangle<int>(
            gestureTargetLabel.getX() - 10, mainTop,
            gestureTargetLabel.getWidth() + 20,
            automaticRow.getBottom() - mainTop + 10);

        gestureArea.removeFromTop(compactGestures ? 10 : 20);
        gesturesPedalPanelBounds = gestureArea;
        auto pedalArea = gestureArea.reduced(
            compactGestures ? 14 : 22, compactGestures ? 8 : 12);
        sustainMonitorLabel.setBounds(pedalArea.removeFromTop(
            compactGestures ? 32 : 48));
        pedalArea.removeFromTop(compactGestures ? 3 : 5);
        saxFootswitchBindingLabel.setBounds(pedalArea.removeFromTop(
            compactGestures ? 36 : 54));
        pedalArea.removeFromTop(compactGestures ? 5 : 8);
        auto pedalButtons = pedalArea.removeFromTop(
            juce::jmin(compactGestures ? 50 : 74,
                       pedalArea.getHeight()));
        saxFootswitchClearButton.setBounds(
            pedalButtons.removeFromRight(190).reduced(3));
        pedalButtons.removeFromRight(12);
        saxFootswitchLearnButton.setBounds(pedalButtons.reduced(3));
        return;
    }

    auto performanceArea = bounds.reduced(4, 6);
    constexpr int gap = 20;
    const auto controlsHeight = juce::jlimit(
        148, 176, performanceArea.getHeight() / 4);
    auto controlsArea = performanceArea.removeFromBottom(controlsHeight);
    const auto controlRowGap = 12;
    const auto controlRowHeight
        = (controlsArea.getHeight() - controlRowGap) / 2;
    auto levelArea = controlsArea.removeFromTop(controlRowHeight).reduced(5, 4);
    controlsArea.removeFromTop(controlRowGap);
    auto delayArea = controlsArea.reduced(5, 4);
    performanceArea.removeFromBottom(14);
    const auto controlLabelWidth = juce::jlimit(
        220, 285, levelArea.getWidth() * 15 / 100);
    performanceLevelLabel.setBounds(
        levelArea.removeFromLeft(controlLabelWidth));
    levelArea.removeFromLeft(16);
    resetPerformanceLevelButton.setBounds(
        levelArea.removeFromRight(120).reduced(3, 2));
    performanceLevelSlider.setBounds(levelArea.reduced(2, 0));
    delayLevelLabel.setBounds(delayArea.removeFromLeft(controlLabelWidth));
    delayArea.removeFromLeft(16);
    toggleDelayDryButton.setBounds(
        delayArea.removeFromRight(150).reduced(3, 2));
    delayLevelSlider.setBounds(delayArea.reduced(2, 0));
    const auto saxHeight = juce::jlimit(220, 260,
        static_cast<int>(static_cast<float>(performanceArea.getHeight()) * 0.31f));
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
    const auto saxControlsWidth = juce::jlimit(430, 620,
        static_cast<int>(static_cast<float>(saxArea.getWidth()) * 0.34f));
    saxControlPanelBounds = saxArea.removeFromRight(saxControlsWidth);
    saxArea.removeFromRight(gap);
    orbs[static_cast<size_t>(EcosystemEngine::midiMemoryCount)]->setBounds(
        saxArea);

    auto saxControls = saxControlPanelBounds.reduced(26, 20);
    saxModeButton.setBounds(saxControls.removeFromTop(64));
    saxControls.removeFromTop(14);
    decayLabel.setBounds(saxControls.removeFromTop(32));
    decaySlider.setBounds(saxControls.removeFromTop(58));
}
